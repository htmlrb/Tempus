/**
 *   Copyright (C) 2012-2013 IFSTTAR (http://www.ifsttar.fr)
 *   Copyright (C) 2012-2013 Oslandia <infos@oslandia.com>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 * 
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.
 */

#include "dynamic_multi_plugin.hh"
#include <boost/format.hpp>
#include <boost/graph/dijkstra_shortest_paths.hpp>
#include <boost/graph/graphviz.hpp>

#include "pgsql_importer.hh"
#include "utils/timer.hh"
#include "utils/field_property_accessor.hh"
#include "utils/function_property_accessor.hh"
#include "utils/associative_property_map_default_value.hh"

#include "datetime.hh"
#include "cost_lib/cost_calculator.hh"
#include "automaton_lib/automaton.hh"
#include "algorithms.hh"
#include "reverse_multimodal_graph.hh"
#include "utils/graph_db_link.hh"

using namespace std;

namespace Tempus {

// the exception used to shortcut dijkstra
struct path_found_exception
{
    path_found_exception( const Triple& t ) : destination(t) {}
    Triple destination;
};

// special visitor to shortcut dijkstra
template <class Graph>
class DestinationDetectorVisitor
{
public:
    DestinationDetectorVisitor( const Graph& graph,
                                const std::vector<Road::Vertex>& destinations,
                                bool pvad,
                                bool verbose,
                                int& iterations) :
        graph_(graph),
        pvad_(pvad),
        verbose_(verbose),
        iterations_(iterations)
    {
        for ( size_t i = 0; i < destinations.size(); i++ ) {
            destinations_.insert(destinations[i]);
        }
    }

    void examine_vertex( const Triple& t, const Graph& )
    {
        for ( std::set<Road::Vertex>::const_iterator it = destinations_.begin(); it != destinations_.end(); ++it ) {
            if ( t.vertex.type() == Multimodal::Vertex::Road && t.vertex.road_vertex() == *it ) {
                TransportMode mode = graph_.transport_mode( t.mode ).get();
                bool found = false;
                if ( !mode.must_be_returned() && !mode.is_public_transport() ) {
                    if ( (t.mode == TransportModePrivateCar) || (t.mode == TransportModePrivateBicycle) ) {
                        if ( pvad_ ) {
                            found = true;
                        }
                    }
                    else {
                        found = true;
                    }
                }
                if (found) {
                    // one destination less
                    std::cout << "found destination " << *it << std::endl;
                    destinations_.erase(it);
                    if ( destinations_.empty() ) {
                        throw path_found_exception( t );
                    }
                }
            }
        }
        if (verbose_) {
            std::cout << "Examine triple (vertex=" << t.vertex << ", mode=" << t.mode << ", state=" << t.state << ")" << std::endl;
        }
        iterations_++;
    }

    void discover_vertex( const Triple&, const Graph& )
    {
    }

    void finish_vertex( const Triple&, const Graph& )
    {
    }

    void examine_edge( const Multimodal::Edge& e, const Graph& )
    {
        if (verbose_) {
            std::cout << "Examine edge " << e << std::endl;
        }
    }

    void edge_relaxed( const Multimodal::Edge& e, unsigned int mode, const Graph& )
    {
        if (verbose_) {
            std::cout << "Edge relaxed " << e << " mode " << mode << std::endl;
        }
    }
    void edge_not_relaxed( const Multimodal::Edge& e, unsigned int mode, const Graph& )
    {
        if (verbose_) {
            std::cout << "Edge not relaxed " << e << " mode " << mode << std::endl;
        }
    }

private:
    const Graph& graph_;
    std::set<Road::Vertex> destinations_;
    // private vehicule at destination
    bool pvad_;
    bool verbose_;
    int& iterations_;
};

template <class Graph>
struct EuclidianHeuristic
{
public:
    EuclidianHeuristic( const Graph& g, const Road::Vertex& destination, double max_speed = 0.06 ) :
        graph_(g), max_speed_(max_speed)
    {
        destination_ = g.road()[destination].coordinates();
    }

    double operator()( const Multimodal::Vertex& v )
    {
        // max speed = 130 km/h (in m/min)
        return distance( destination_, v.coordinates() ) / (max_speed_ * 1000.0 / 60.0);
    }
private:
    const Graph& graph_;
    Point3D destination_;
    double max_speed_;
};

struct NullHeuristic
{
    double operator()( const Multimodal::Vertex& )
    {
        return 0.0;
    }
};

const DynamicMultiPlugin::OptionDescriptionList DynamicMultiPlugin::option_descriptions()
{
    Plugin::OptionDescriptionList odl;
    odl.declare_option( "Features/with_forbidden_turning_movements", "With forbidden turning movements", true);
    odl.declare_option( "Features/timetable_frequency", "From timetables (0), frequencies (1) travel time estimation", 0);
    odl.declare_option( "Debug/verbose_algo", "Verbose algorithm: vertices and edges traversal", false);
    odl.declare_option( "Debug/verbose", "Verbose general processing", true);
    odl.declare_option( "Debug/enable_trace", "Produce a trace (warning: cpu and time consuming)", false );
    odl.declare_option( "Time/min_transfer_time", "Minimum time necessary for a transfer to be done (in min)", 2);
    odl.declare_option( "Time/walking_speed", "Average walking speed (km/h)", 3.6);
    odl.declare_option( "Time/cycling_speed", "Average cycling speed (km/h)", 12);
    odl.declare_option( "Time/car_parking_search_time", "Car parking search time (min)", 5);
    odl.declare_option( "AStar/heuristic", "Use an heuristic based on euclidian distance", false );
    odl.declare_option( "AStar/speed_heuristic", "Max speed (km/h) to use in the heuristic", 0.06 );
    odl.declare_option( "Time/use_speed_profiles", "Use road speed profiles", false );
    odl.declare_option( "multi_destinations", "Destination list (road vertex id, comma separated)", std::string("") );
    return odl;
}

const DynamicMultiPlugin::PluginCapabilities DynamicMultiPlugin::plugin_capabilities()
{
    Plugin::PluginCapabilities caps;
    caps.optimization_criteria().push_back( CostDuration );
    caps.set_depart_after( true );
    caps.set_arrive_before( true );
    return caps;
}

void DynamicMultiPlugin::post_build()
{
    const Road::Graph& road_graph = Application::instance()->graph()->road();
		
    PQImporter psql( Application::instance()->db_options() ); 
    Road::Restrictions restrictions = psql.import_turn_restrictions( road_graph, Application::instance()->schema_name() );
    std::cout << "Turn restrictions imported" << std::endl;
		
    s_.automaton.build_graph( restrictions ) ;
    std::cout << "Automaton built" << std::endl; 
		
    std::ofstream ofs("all_movements_edge_automaton.dot"); // output to graphviz
    Automaton<Road::Edge>::NodeWriter nodeWriter( s_.automaton.automaton_graph_ );
    Automaton<Road::Edge>::ArcWriter arcWriter( s_.automaton.automaton_graph_, road_graph );
    //    boost::write_graphviz( ofs, s_.automaton.automaton_graph_, nodeWriter, arcWriter);
    std::cout << "Automaton exported to graphviz" << std::endl; 
}
    
void DynamicMultiPlugin::pre_process( Request& request )
{
    // Initialize metrics
    time_=0; 
    time_algo_=0; 
    iterations_=0; 
    	
    // Initialize timer for preprocessing time
    Timer timer; 
    	
    // Check request and clear result
    REQUIRE( request.allowed_modes().size() >= 1 );
    REQUIRE( vertex_exists( request.origin(), graph_.road() ) );
    REQUIRE( vertex_exists( request.destination(), graph_.road() ) );
    request_ = request; 

    if ( request.optimizing_criteria()[0] != CostDuration ) 
        throw std::invalid_argument( "Unsupported optimizing criterion" ); 
        
    if ( verbose_ ) cout << "Road origin node ID = " << graph_.road()[request_.origin()].db_id() << ", road destination node ID = " << graph_.road()[request_.destination()].db_id() << endl;
    result_.clear(); 
            	
    // Get plugin options 		
    get_option( "Debug/verbose", verbose_ ); 
    get_option( "Debug/verbose_algo", verbose_algo_ ); 
    get_option( "Debug/enable_trace", enable_trace_ );
    get_option( "Features/timetable_frequency", timetable_frequency_ ); 
    get_option( "Time/min_transfer_time", min_transfer_time_ ); 
    get_option( "Time/walking_speed", walking_speed_ ); 
    get_option( "Time/cycling_speed", cycling_speed_ ); 
    get_option( "Time/car_parking_search_time", car_parking_search_time_ );
    get_option( "Time/use_speed_profiles", use_speed_profiles_ );

    // look for public transports in allowed modes
    bool pt_allowed = false;
    // look also for private modes
    bool private_mode = false;
    for ( size_t i = 0; i < request.allowed_modes().size(); i++ ) {
        db_id_t mode_id = request.allowed_modes()[i];
        if (graph_.transport_mode( mode_id )->is_public_transport() ) {
            pt_allowed = true;
        }
        else if ( (mode_id == TransportModePrivateBicycle) || (mode_id == TransportModePrivateCar) ) {
            private_mode = true;
        }
    }
    // resolve private parking location
    if ( private_mode ) {
        if ( request_.parking_location() ) {
            parking_location_ = request_.parking_location().get();
        }
        else {
            // place the private parking at the origin
            parking_location_ = request_.origin();
        }
    }

    if ( use_speed_profiles_ && (request_.steps()[1].constraint().type() != Request::TimeConstraint::ConstraintAfter) ) {
        throw std::runtime_error( "A 'depart after' constraint must be specified for speed profiles" );
    }
        
    // If current date changed, reload timetable / frequency
    if (
        ( (request_.steps()[1].constraint().type() == Request::TimeConstraint::ConstraintAfter) ||
          (request_.steps()[1].constraint().type() == Request::TimeConstraint::ConstraintBefore) ) && 
        (s_.current_day != request_.steps()[1].constraint().date_time().date())
        ) {
        if ( pt_allowed && (graph_.public_transports().size() > 0) ) {
            const PublicTransport::Graph& pt_graph = *graph_.public_transports().begin()->second;
            std::cout << "Load timetable" << std::endl;
            s_.current_day = request_.steps()[1].constraint().date_time().date();

            // cache graph id to descriptor
            // FIXME - integrate the cache into the graphs ?
            std::map<db_id_t, PublicTransport::Vertex> vertex_from_id_;
            PublicTransport::Graph::vertex_iterator vit, vend;
            for ( boost::tie( vit, vend ) = vertices( pt_graph ); vit != vend; ++vit ) {
                vertex_from_id_[ pt_graph[*vit].db_id() ] = *vit;
            }

            if (timetable_frequency_ == 0) // timetable model 
            {
                // Charging necessary timetable data for request processing
                Db::Result res = db_.exec( ( boost::format( "SELECT t1.stop_id as origin_stop, t2.stop_id as destination_stop, "
                                                            "t1.trip_id, pt_route.transport_mode, extract(epoch from t1.arrival_time)/60 as departure_time, extract(epoch from t2.departure_time)/60 as arrival_time "
                                                            "FROM tempus.pt_stop_time t1, tempus.pt_stop_time t2, tempus.pt_trip, tempus.pt_route "
                                                            "WHERE t1.trip_id = t2.trip_id AND t1.stop_sequence + 1 = t2.stop_sequence AND pt_trip.id = t1.trip_id "
                                                            "AND pt_route.id = pt_trip.route_id "
                                                            "AND pt_trip.service_id IN "
                                                            "("
                                                            "	(WITH calend AS ("
                                                            "		SELECT service_id, start_date, end_date, ARRAY[sunday, monday, tuesday, wednesday, thursday, friday, saturday] AS days "
                                                            "		FROM tempus.pt_calendar	)"
                                                            "	SELECT service_id "
                                                            "		FROM calend "
                                                            "		WHERE days[(SELECT EXTRACT(dow FROM TIMESTAMP '%1%')+1)] AND start_date <= '%1%' AND end_date >= '%1%' ) "
                                                            "EXCEPT ( "
                                                            "	SELECT service_id "
                                                            "		FROM tempus.pt_calendar_date "
                                                            "		WHERE calendar_date='%1%' AND exception_type=2"
                                                            "	) "
                                                            "UNION ("
                                                            "	SELECT service_id "
                                                            "		FROM tempus.pt_calendar_date	"
                                                            "		WHERE calendar_date='%1%' AND exception_type=1"
                                                            "	)"
                                                            ")") % boost::gregorian::to_simple_string(s_.current_day) ).str() ); 
				
                for ( size_t i = 0; i < res.size(); i++ ) {
                    PublicTransport::Vertex departure, arrival; 
                    PublicTransport::Edge e; 
                    bool found;
                    departure = vertex_from_id_[ res[i][0].as<db_id_t>() ];
                    arrival = vertex_from_id_[ res[i][1].as<db_id_t>() ]; 
                    boost::tie( e, found ) = boost::edge( departure, arrival, pt_graph );
                    TimetableData t, rt; 
                    t.trip_id = res[i][2].as<int>();
                    int mode_id = res[i][3].as<int>();
                    double departure_time = res[i][4].as<double>();
                    double arrival_time = res[i][5].as<double>();
                    t.arrival_time = arrival_time;
                    s_.timetable.insert( std::make_pair(e, std::map<int, std::map<double, TimetableData> >() ) );
                    s_.timetable[e].insert( std::make_pair( mode_id, std::map<double, TimetableData>() ));
                    s_.timetable[e][mode_id].insert( std::make_pair( departure_time, t ) );

                    // reverse timetable
                    rt.trip_id = t.trip_id;
                    rt.arrival_time = departure_time;

                    s_.rtimetable.insert( std::make_pair(e, std::map<int, std::map<double, TimetableData> >() ) );
                    s_.rtimetable[e].insert( std::make_pair( mode_id, std::map<double, TimetableData>() ));
                    s_.rtimetable[e][mode_id].insert( std::make_pair( arrival_time, rt ) );
                }
            }
            else if (timetable_frequency_ == 1) // frequency model
            {
                // Charging necessary frequency data for request processing 
                Db::Result res = db_.exec( ( boost::format( "SELECT t1.stop_id as origin_stop, t2.stop_id as destination_stop, "
                                                            "t1.trip_id, pt_route.transport_mode, extract(epoch from pt_frequency.start_time)/60 as start_time, extract(epoch from pt_frequency.end_time)/60 as end_time, "
                                                            "pt_frequency.headway_secs/60 as headway, extract(epoch from t2.departure_time - t1.arrival_time)/60 as travel_time "
                                                            "FROM tempus.pt_stop_time t1, tempus.pt_stop_time t2, tempus.pt_trip, tempus.pt_frequency, tempus.pt_route "
                                                            "WHERE t1.trip_id=t2.trip_id AND t1.stop_sequence + 1 = t2.stop_sequence AND pt_trip.id=t1.trip_id AND pt_frequency.trip_id = t1.trip_id "
                                                            "AND pt_route.id = pt_trip.route_id "
                                                            "AND pt_trip.service_id IN "
                                                            "("
                                                            "	(WITH calend AS ("
                                                            "		SELECT service_id, start_date, end_date, ARRAY[sunday, monday, tuesday, wednesday, thursday, friday, saturday] AS days "
                                                            "		FROM tempus.pt_calendar	)"
                                                            "	SELECT service_id "
                                                            "		FROM calend "
                                                            "		WHERE days[(SELECT EXTRACT(dow FROM TIMESTAMP '%1%')+1)] AND start_date <= '%1%' AND end_date >= '%1%' ) "
                                                            "EXCEPT ( "
                                                            "	SELECT service_id "
                                                            "		FROM tempus.pt_calendar_date "
                                                            "		WHERE calendar_date='%1%' AND exception_type=2"
                                                            "	) "
                                                            "UNION ("
                                                            "	SELECT service_id "
                                                            "		FROM tempus.pt_calendar_date	"
                                                            "		WHERE calendar_date='%1%' AND exception_type=1"
                                                            "	)"
                                                            ")") % boost::gregorian::to_simple_string(s_.current_day) ).str() ); 
                for ( size_t i = 0; i < res.size(); i++ ) {
                    PublicTransport::Vertex departure, arrival; 
                    PublicTransport::Edge e; 
                    bool found; 
                    departure = vertex_from_id_[ res[i][0].as<db_id_t>() ];
                    arrival = vertex_from_id_[ res[i][1].as<db_id_t>() ];
                    boost::tie( e, found ) = boost::edge( departure, arrival, pt_graph );

                    FrequencyData f; 
                    f.trip_id = res[i][2].as<int>();
                    int mode_id = res[i][3].as<int>();
                    f.end_time = res[i][5].as<double>();
                    f.headway = res[i][6].as<double>();
                    f.travel_time = res[i][7].as<double>();
                    double start_time = res[i][4].as<double>();

                    s_.frequency.insert( std::make_pair(e, map<int, std::map<double, FrequencyData> >() ) );
                    s_.frequency[e].insert( std::make_pair( mode_id, std::map<double, FrequencyData>() ) );
                    s_.frequency[e][mode_id].insert( std::make_pair( start_time, f ) );

                    // reverse frequency data
                    FrequencyData rf;
                    rf.trip_id = f.trip_id;
                    rf.end_time = start_time;
                    rf.headway = f.headway;
                    rf.travel_time = f.travel_time;

                    s_.rfrequency.insert( std::make_pair(e, map<int, std::map<double, FrequencyData> >() ) );
                    s_.rfrequency[e].insert( std::make_pair( mode_id, std::map<double, FrequencyData>() ) );
                    s_.rfrequency[e][mode_id].insert( std::make_pair( f.end_time, rf ) );
                }
            }
        }
    
        std::cout << "Load speed profiles" << std::endl;
        // load speed profiles
        // FIXME: must take day into account
        Db::Result res = db_.exec("SELECT road_section_id, speed_rule, begin_time, end_time, average_speed FROM\n"
                                  "tempus.road_section_speed as ss,\n"
                                  "tempus.road_daily_profile as p\n"
                                  "WHERE\n"
                                  "p.profile_id = ss.profile_id");
            
        for ( size_t i = 0; i < res.size(); i++ ) {
            db_id_t road_section = res[i][0].as<db_id_t>();
            int speed_rule = res[i][1].as<int>();
            double begin_time = res[i][2].as<double>();
            double end_time = res[i][3].as<double>();
            double speed = res[i][4].as<double>();
            
            s_.speed_profile.add_period( road_section, static_cast<TransportModeSpeedRule>(speed_rule), begin_time, end_time-begin_time, speed );
        }
    }
        
    // Update timer for preprocessing
    time_+=timer.elapsed();
}
        
void DynamicMultiPlugin::process()
{
    // Initialize timer for algo processing time
    Timer timer;

    // Get origin and destination nodes
    Multimodal::Vertex origin = Multimodal::Vertex( &graph_.road(), request_.origin() );
    destination_ = Multimodal::Vertex( &graph_.road(), request_.destination() );

    bool reversed = request_.steps().back().constraint().type() == Request::TimeConstraint::ConstraintBefore;

    Triple origin_o;
    origin_o.vertex = origin;
    origin_o.state = s_.automaton.initial_state_;
    // take the first mode allowed
    origin_o.mode = request_.allowed_modes()[0];

    Triple destination_o;
    destination_o.vertex = destination_;
    destination_o.state = s_.automaton.initial_state_;
    destination_o.mode = request_.allowed_modes()[0];

    // Initialize the potential map
    // adaptation to a property map : infinite default value
    associative_property_map_default_value< PotentialMap > potential_pmap( potential_map_, std::numeric_limits<double>::max() );
        
    // Initialize the predecessor map
    boost::associative_property_map< PredecessorMap > pred_pmap( pred_map_ );  // adaptation to a property map

    double start_time = request_.steps()[1].constraint().date_time().time_of_day().total_seconds()/60;

    if ( !reversed ) {
        put( potential_pmap, origin_o, start_time ) ;
        put( pred_pmap, origin_o, origin_o );
    }
    else {
        put( potential_pmap, destination_o, -start_time ) ;
        put( pred_pmap, destination_o, destination_o );
    }

    // Initialize the trip map
    associative_property_map_default_value< TripMap > trip_pmap( trip_map_, 0 );

    // Initialize the wait map
    boost::associative_property_map< PotentialMap > wait_pmap( wait_map_ );
    boost::associative_property_map< PotentialMap > shift_pmap( shift_map_ );

    // Define and initialize the cost calculator
    const RoadEdgeSpeedProfile* profile;
    if ( use_speed_profiles_ ) {
        profile = &s_.speed_profile;
    }
    else {
        profile = 0;
    }
    CostCalculator cost_calculator( s_.timetable, s_.rtimetable, s_.frequency, s_.rfrequency, request_.allowed_modes(), available_vehicles_, walking_speed_, cycling_speed_, min_transfer_time_, car_parking_search_time_, parking_location_, profile );

    // destinations
    std::vector<Road::Vertex> destinations;
    // if the "multi_destinations" option is here, take destinations from it
    std::string dest_str;
    get_option( "multi_destinations", dest_str );
    if ( !dest_str.empty() ) {
        while (!dest_str.empty()) {
            std::string car, cdr;
            size_t p = dest_str.find(',');
            if ( p == std::string::npos ) {
                car = dest_str;
            }
            else {
                car = dest_str.substr(0,p);
                cdr = dest_str.substr(p+1);
            }
            db_id_t vidx;
            try {
                vidx = boost::lexical_cast<db_id_t>(car);
            }
            catch (std::exception&) {
                throw std::runtime_error("Cannot parse " + car );
            }
            bool found;
            Road::Vertex v;
            boost::tie(v, found) = vertex_from_id( vidx, graph_.road() );
            if ( !found ) {
                throw std::runtime_error("Cannot find vertex from " + car );
            }
            destinations.push_back(v);
            dest_str = cdr;
        }
    }
    else {
        if (!reversed) {
            destinations.push_back( request_.destination() );
        }
        else {
            destinations.push_back( request_.origin() );
        }
    }

    // we cannot use the regular visitor here, since we examine tuples instead of vertices
    DestinationDetectorVisitor<Multimodal::Graph> vis( graph_, destinations, request_.steps().back().private_vehicule_at_destination(), verbose_algo_, iterations_ );

    bool path_found = false;
    try {
        bool use_heuristic;
        get_option( "AStar/heuristic", use_heuristic );
        if ( use_heuristic ) {
            double h_speed_max;
            get_option( "AStar/speed_heuristic", h_speed_max );

            if ( reversed ) {
                Multimodal::ReverseGraph rgraph( graph_ );
                EuclidianHeuristic<Multimodal::ReverseGraph> heuristic( rgraph, request_.origin(), h_speed_max );
                DestinationDetectorVisitor<Multimodal::ReverseGraph> rvis( rgraph, destinations, request_.steps().back().private_vehicule_at_destination(), verbose_algo_, iterations_ );
                combined_ls_algorithm_no_init( rgraph, s_.automaton, destination_o, pred_pmap, potential_pmap, cost_calculator, trip_pmap, wait_pmap, shift_pmap, request_.allowed_modes(), rvis, heuristic );
            }
            else {
                EuclidianHeuristic<Multimodal::Graph> heuristic( graph_, request_.destination(), h_speed_max );
                combined_ls_algorithm_no_init( graph_, s_.automaton, origin_o, pred_pmap, potential_pmap, cost_calculator, trip_pmap, wait_pmap, shift_pmap, request_.allowed_modes(), vis, heuristic );
            }
        }
        else {
            if ( reversed ) {
                Multimodal::ReverseGraph rgraph( graph_ );
                DestinationDetectorVisitor<Multimodal::ReverseGraph> rvis( rgraph, destinations, request_.steps().back().private_vehicule_at_destination(), verbose_algo_, iterations_ );
                combined_ls_algorithm_no_init( rgraph, s_.automaton, destination_o, pred_pmap, potential_pmap, cost_calculator, trip_pmap, wait_pmap, shift_pmap, request_.allowed_modes(), rvis, NullHeuristic() );
            }
            else {
                EuclidianHeuristic<Multimodal::Graph> heuristic( graph_, request_.destination() );
                combined_ls_algorithm_no_init( graph_, s_.automaton, origin_o, pred_pmap, potential_pmap, cost_calculator, trip_pmap, wait_pmap, shift_pmap, request_.allowed_modes(), vis, NullHeuristic() );
            }
        }
    }
    catch ( path_found_exception& e ) {
        // Dijkstra has been short cut when the destination node is reached
        if ( !reversed ) {
            destination_o = e.destination;
        }
        else {
            origin_o = e.destination;
        }
        path_found = true;
    }
        
    metrics_[ "iterations" ] = iterations_;
		
    time_ += timer.elapsed();
    time_algo_ += timer.elapsed();
    metrics_[ "time_s" ] = time_;
    metrics_[ "time_algo_s" ] = time_algo_;

    if (!path_found) {
        throw std::runtime_error( "No path found" );                
    }

    if ( !reversed ) {
        Path path = reorder_path( origin_o, destination_o );
        add_roadmap( path );
    }
    else {
        Path path = reorder_path( destination_o, origin_o, /* reverse */ true );
        add_roadmap( path, /* reverse */ true );
    }
}
    
Path DynamicMultiPlugin::reorder_path( Triple departure, Triple arrival, bool reverse )
{
    Path path; 
    Triple current = arrival;
    bool found = true; 

    while ( current.vertex != departure.vertex ) {
        if (!reverse) {
            path.push_front( current );
        }
        else {
            path.push_back( current );
        }

        if ( pred_map_[ current ] == current ) {
            found = false;
            break;
        }

        current = pred_map_[ current ]; 
    }

    if ( !found ) {
        throw std::runtime_error( "No path found" );
    }

    if (!reverse) {
        path.push_front( departure );
    }
    else {
        path.push_back( departure );
    }
    return path; 
}

std::string sec2hm( double s )
{
    double s2 = s < 0 ? -s: s;
    return (s<0?"-":"") + (boost::format("%02d:%02d:%02d") % (int(s2)/60) % (int(s2) % 60) % int((s2-int(s2)) * 60)).str();
}
    
void DynamicMultiPlugin::add_roadmap( const Path& path, bool reverse )
{
    result_.push_back( Roadmap() ); 
    Roadmap& roadmap = result_.back();

    std::list< Triple >::const_iterator it = path.begin();
    std::list< Triple >::const_iterator next = it;
    next++;

    double start_time;
    if (reverse) {
        //
        // first pass to reverse the steps and get the right times and waiting times

        double wait_time = 0.0;
        PotentialMap new_wait_map;
        double shift = shift_map_[*it];
        double to_shift = 0.0;
        potential_map_[*it] = -potential_map_[*it] - shift;
        start_time = potential_map_[*it];
        int n_pt_edges = 0;
        for ( ; next != path.end(); ++next, ++it ) {
            // std::cout << it->vertex << "\tP: " << sec2hm(potential_map_[*it]) << " W: " << sec2hm(wait_map_[*it]) << " shift_map: " << sec2hm(shift_map_[*it]) << std::endl;
            //            std::cout << next->vertex << "\t  P: " << sec2hm(potential_map_[*next]) << " W: " << sec2hm(wait_map_[*next]) << " S: " << sec2hm(shift) << " wait_time: " << wait_time << std::endl;

            double h1 = potential_map_[*it];
            double h2 = -potential_map_[*next] - shift;
            if ( it->vertex.type() == Multimodal::Vertex::Road && next->vertex.type() == Multimodal::Vertex::PublicTransport ) {
                if ( wait_map_[*it] > 0.0 ) {
                    // the waiting time is stored here, but must be moved to the next PT2PT
                    // add it to the current wait_time (from a previous PT2PT)
                    wait_time += wait_map_[*it];
                }
                new_wait_map[*it] = 0.0;
                n_pt_edges = 0;
            }
            else if ( it->vertex.type() == Multimodal::Vertex::PublicTransport && next->vertex.type() == Multimodal::Vertex::PublicTransport ) {
                if ( n_pt_edges == 0 ) {
                    if ( wait_time > 0.0 ) {
                        // we have a wait_time, move potential of the current vertex
                        h1 -= wait_time;
                        // store it at the right place
                        new_wait_map[*next] = wait_time;
                        wait_time = 0.0;
                    }
                    if ( to_shift > 0.0 ) {
                        // we need to advance time
                        shift -= to_shift;
                        h1 += to_shift;
                        h2 += to_shift;
                        to_shift = 0.0;
                    }
                }
                if ( wait_map_[*it] > 0.0 ) {
                    // if we also have a wait_time here, it means we will need to shift time for the next PT2PT
                    wait_time = wait_map_[*it];
                    to_shift = wait_time;
                }

                // store the trip_id at the right place
                trip_map_[*next] = trip_map_[*it];

                n_pt_edges++;
            }
            else {
                n_pt_edges = 0;
            }
            potential_map_[*it] = h1;
            potential_map_[*next] = h2;
        }
        it = path.begin();
        next = it;
        next++;

        wait_map_ = new_wait_map;
    }
    else {
        start_time = request_.steps()[1].constraint().date_time().time_of_day().total_seconds()/60;
    }

    roadmap.set_starting_date_time( request_.steps()[1].constraint().date_time() );
    std::cout << "resulting start_time: " << sec2hm(start_time) << std::endl;

    std::cout << it->vertex << "\tP: " << sec2hm(potential_map_[*it]) << " W: " << sec2hm(wait_map_[*it]) << std::endl;
    double total_duration = 0.0;
    for ( ; next != path.end(); ++next, ++it ) {
        std::auto_ptr<Roadmap::Step> mstep;

        std::cout << next->vertex << "\tP: " << sec2hm(potential_map_[*next]) << " W: " << sec2hm(wait_map_[*next]) << std::endl;

        if ( it->mode == next->mode && it->vertex.type() == Multimodal::Vertex::Road && next->vertex.type() == Multimodal::Vertex::Road ) {
            mstep.reset( new Roadmap::RoadStep() );
            Roadmap::RoadStep* step = static_cast<Roadmap::RoadStep*>( mstep.get() );
            Road::Edge e;
            bool found = false;
            boost::tie( e, found ) = edge( it->vertex.road_vertex(), next->vertex.road_vertex(), *it->vertex.road_graph() ); 

            if ( !found ) {
                throw std::runtime_error( (boost::format("Can't find the road edge ! %d %d") % it->vertex.road_vertex() % next->vertex.road_vertex()).str() );
            }

            step->set_road_edge( e );
            step->set_transport_mode( it->mode );
            step->set_cost( CostDuration, potential_map_[ *next ] - potential_map_[ *it ] );
        }

        else if ( it->vertex.type() == Multimodal::Vertex::PublicTransport && next->vertex.type() == Multimodal::Vertex::PublicTransport ) {
            mstep.reset( new Roadmap::PublicTransportStep() );
            Roadmap::PublicTransportStep* step = static_cast<Roadmap::PublicTransportStep*>( mstep.get() );
				
            step->set_transport_mode( it->mode );
            step->set_departure_stop( it->vertex.pt_vertex() );
            step->set_arrival_stop( next->vertex.pt_vertex() );

            step->set_departure_time( potential_map_[*it] );
            step->set_arrival_time( potential_map_[*next] );
            step->set_trip_id(trip_map_[ *next ]);
            step->set_wait(wait_map_[ *next ]);
				
            // find the network_id
            for ( Multimodal::Graph::PublicTransportGraphList::const_iterator nit = graph_.public_transports().begin(); nit != graph_.public_transports().end(); ++nit ) {
                if ( it->vertex.pt_graph() == nit->second ) {
                    step->set_network_id(nit->first);
                    break;
                }
            }
            step->set_cost( CostDuration, step->arrival_time() - step->departure_time() );
        }
        else {
            // Make a multimodal edge and copy it into the roadmap as a 'generic' step
            mstep.reset( new Roadmap::TransferStep( Multimodal::Edge( it->vertex, next->vertex ) ) );
            Roadmap::TransferStep* step = static_cast<Roadmap::TransferStep*>(mstep.get());
            step->set_transport_mode( it->mode );
            step->set_final_mode( next->mode );
            step->set_cost( CostDuration, potential_map_[ *next ] - potential_map_[ *it ] );
        }
        total_duration += mstep->cost( CostDuration );

        roadmap.add_step( mstep );
    }

    if (reverse) {
        // set starting time
        int mins = int(start_time);
        int secs = int((start_time - mins) * 60.0);
        DateTime dt( request_.steps().back().constraint().date_time().date(),
                     boost::posix_time::minutes( mins ) + boost::posix_time::seconds( secs ) );
        roadmap.set_starting_date_time( dt );
    }

    // populate the path parts, if needed
    if (enable_trace_) {
        PathTrace trace;
        for ( PredecessorMap::const_iterator vit = pred_map_.begin(); vit != pred_map_.end(); vit++ ) {
            if ( vit->second.vertex == vit->first.vertex ) {
                continue;
            }
            Triple o, d;
            if ( reverse ) {
                o = vit->first;
                d = vit->second;
            }
            else {
                o = vit->second;
                d = vit->first;
            }

            ValuedEdge ve( o.vertex, d.vertex );
            ve.set_value( "duration", potential_map_[d] );
            ve.set_value( "imode", int(o.mode) );
            ve.set_value( "fmode", int(d.mode) );
            ve.set_value( "istate", int(o.state) );
            ve.set_value( "fstate", int(d.state) );
            trace.push_back( ve );
        }
        roadmap.set_trace( trace );
    }
}

StaticVariables DynamicMultiPlugin::s_;
} 

DECLARE_TEMPUS_PLUGIN( "dynamic_multi_plugin", Tempus::DynamicMultiPlugin )




