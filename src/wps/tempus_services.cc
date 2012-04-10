#include <iostream>

#include "tempus_services.hh"
#include "roadmap.hh"

using namespace std;

using Tempus::Roadmap;

namespace WPS
{    
    PreBuildService::PreBuildService() : Service("pre_build")
    {
	// define the XML schema of input parameters
	add_input_parameter( "db_options",
			     "<xs:element name=\"db_options\" type=\"xs:string\"/>" );
    }
    void PreBuildService::parse_xml_parameters( Service::ParameterMap& input_parameter_map )
    {
	// Ensure XML is OK
	Service::check_parameters( input_parameter_map, input_parameter_schema_ );
	
	// now extract actual data
	xmlNode* request_node = input_parameter_map["db_options"];
	db_options_ = (const char*)request_node->children->content;
    }

    Service::ParameterMap& PreBuildService::execute()
    {
	plugin_->pre_build( db_options_ );
	output_parameters_.clear();
	return output_parameters_;
    }

    BuildService::BuildService() : Service("build")
    {
    }
    Service::ParameterMap& BuildService::execute()
    {
	plugin_->build();
	output_parameters_.clear();
	return output_parameters_;
    }

    PreProcessService::PreProcessService() : Service("pre_process"), Tempus::Request()
    {
	// define the XML schema of input parameters
	add_input_parameter( "request",
			     "  <xs:complexType name=\"TimeConstraint\">\n"
			     "    <xs:attribute name=\"type\" type=\"xs:int\"/>\n"
			     "    <xs:attribute name=\"date_time\" type=\"xs:dateTime\"/>\n"
			     "  </xs:complexType>\n"
			     "  <xs:complexType name=\"Step\">\n"
			     "    <xs:sequence>\n"
			     "      <xs:element name=\"destination_id\" type=\"xs:long\" minOccurs=\"1\" maxOccurs=\"1\"/>\n"
			     "      <xs:element name=\"constraint\" type=\"TimeConstraint\" minOccurs=\"1\" maxOccurs=\"1\"/>\n"
			     "      <xs:element name=\"private_vehicule_at_destination\" type=\"xs:boolean\" minOccurs=\"1\" maxOccurs=\"1\"/>\n"
			     "    </xs:sequence>\n"
			     "  </xs:complexType>\n"
			     "  <xs:complexType name=\"Request\">\n"
			     "    <xs:sequence>\n"
			     "      <xs:element name=\"origin_id\" type=\"xs:long\" minOccurs=\"1\" maxOccurs=\"1\"/>\n"
			     "      <xs:element name=\"departure_constraint\" type=\"TimeConstraint\" minOccurs=\"1\" maxOccurs=\"1\"/>\n"
			     "      <xs:element name=\"parking_location_id\" type=\"xs:long\" minOccurs=\"0\" maxOccurs=\"1\"/>\n"
			     "      <xs:element name=\"optimizing_criterion\" type=\"xs:int\" minOccurs=\"1\"/>\n"
			     "      <xs:element name=\"allowed_transport_types\" type=\"xs:int\" minOccurs=\"1\" maxOccurs=\"1\"/>\n"
			     "      <xs:element name=\"allowed_network\" type=\"xs:long\" minOccurs=\"0\"/>\n"
			     "      <xs:element name=\"step\" type=\"Step\" minOccurs=\"1\"/>\n"
			     "    </xs:sequence>\n"
			     "  </xs:complexType>\n"
			     "<xs:element name=\"request\" type=\"Request\"/>\n"
			     );
    }
    
    void PreProcessService::parse_xml_parameters( ParameterMap& input_parameter_map )
    {
	// Ensure XML is OK
	Service::check_parameters( input_parameter_map, input_parameter_schema_ );
	
	// now extract actual data
	xmlNode* request_node = input_parameter_map["request"];
	cout << "request_node " << request_node->name << endl;
	xmlNode* field = XML::get_next_nontext( request_node->children );
	
	Tempus::Road::Graph& road_graph = plugin_->get_graph().road;
	string origin_str = (const char*)field->children->content;
	Tempus::db_id_t origin_id = boost::lexical_cast<Tempus::db_id_t>( origin_str );
	this->origin = vertex_from_id( origin_id, road_graph );
	cout << "origin " << origin << endl;
	
	// departure_constraint
	// TODO
	field = XML::get_next_nontext( field->next );
	
	// parking location id, optional
	xmlNode *n = XML::get_next_nontext( field->next );
	if ( !xmlStrcmp( n->name, (const xmlChar*)"parking_location_id" ) )
	{
	    Tempus::db_id_t parking_loc_id = boost::lexical_cast<Tempus::db_id_t>( n->children->content );
	    this->parking_location = vertex_from_id( parking_loc_id, road_graph );
	    field = n;
	    cout << "parking_location " << parking_location << endl;
	}
	
	// optimizing criteria
	field = XML::get_next_nontext( field->next );	
	this->optimizing_criteria.push_back( boost::lexical_cast<int>( field->children->content ) );

	// allowed transport types
	field = XML::get_next_nontext( field->next );	
	this->allowed_transport_types = boost::lexical_cast<int>( field->children->content );
	cout << "allowed transport types " << allowed_transport_types << endl;
	
	// allowed networks, 1 .. N
	field = XML::get_next_nontext( field->next );
	while ( !xmlStrcmp( field->name, (const xmlChar *)"allowed_network" ) )
	{
	    Tempus::db_id_t network_id = boost::lexical_cast<Tempus::db_id_t>(field->children->content);
	    this->allowed_networks.push_back( vertex_from_id(network_id, road_graph) );
	    field = XML::get_next_nontext( field->next );
	    cout << "allowed network " << allowed_networks.size() << endl;
	}
	
	// steps, 1 .. N
	steps.clear();
	while ( field )
	{
	    cout << "field " << field->name << endl;
	    this->steps.resize( steps.size() + 1 );
		
	    xmlNode *subfield;
	    // destination id
	    subfield = XML::get_next_nontext( field->children );
	    Tempus::db_id_t destination_id = boost::lexical_cast<Tempus::db_id_t>(subfield->children->content);
	    this->steps.back().destination = vertex_from_id( destination_id, road_graph );
	    cout << "destination " << steps.back().destination << endl;
	    
	    // constraint
	    // TODO
	    subfield = XML::get_next_nontext( subfield->next );
	    
	    // private_vehicule_at_destination
	    subfield = XML::get_next_nontext( subfield->next );
	    string val = (const char*)subfield->children->content;
	    this->steps.back().private_vehicule_at_destination = ( val == "true" );
	    cout << "pvat " << steps.back().private_vehicule_at_destination << endl;
	    
	    // next step
	    field = XML::get_next_nontext( field->next ); 
	}
    }

    Service::ParameterMap& PreProcessService::execute()
    {
	plugin_->pre_process( *this );
	output_parameters_.clear();
	return output_parameters_;
    }

    ProcessService::ProcessService() : Service("process")
    {
    }
    Service::ParameterMap& ProcessService::execute()
    {
	plugin_->process();
	output_parameters_.clear();
	return output_parameters_;
    }

    ResultService::ResultService() : Service( "result" )
    {
	add_output_parameter( "result",
			      "<xs:complexType name=\"DbId\">\n"
			      "  <xs:attribute name=\"id\" type=\"xs:long\"/>\n" // db_id
			      "</xs:complexType>\n"
			      "<xs:complexType name=\"Cost\">\n"
			      "  <xs:attribute name=\"type\" type=\"xs:string\"/>\n"
			      "  <xs:attribute name=\"value\" type=\"xs:float\"/>\n"
			      "</xs:complexType>\n"
			      "<xs:complexType name=\"RoadStep\">\n"
			      "  <xs:sequence>\n"
			      "    <xs:element name=\"road_section\" type=\"DbId\" minOccurs=\"1\" maxOccurs=\"1\"/>\n"
			      "    <xs:element name=\"road_direction\" type=\"DbId\" minOccurs=\"1\" maxOccurs=\"1\"/>\n"
			      "    <xs:element name=\"distance_km\" type=\"xs:double\" minOccurs=\"1\" maxOccurs=\"1\"/>\n"
			      "    <xs:element name=\"end_movement\" type=\"xs:int\" minOccurs=\"1\" maxOccurs=\"1\"/>\n"
			      "    <xs:element name=\"cost\" type=\"Cost\" maxOccurs=\"unbounded\"/>\n" // 0..N costs
			      "  </xs:sequence>\n"
			      "</xs:complexType>\n"
			      "<xs:complexType name=\"PublicTransportStep\">\n"
			      "  <xs:sequence>\n"
			      "    <xs:element name=\"departure_stop\" type=\"DbId\" minOccurs=\"1\" maxOccurs=\"1\"/>\n"
			      "    <xs:element name=\"arrival_stop\" type=\"DbId\" minOccurs=\"1\" maxOccurs=\"1\"/>\n"
			      "    <xs:element name=\"trip\" type=\"DbId\" minOccurs=\"1\" maxOccurs=\"1\"/>\n"
			      "    <xs:element name=\"cost\" type=\"Cost\" maxOccurs=\"unbounded\"/>\n" // 0..N costs
			      "  </xs:sequence>\n"
			      "</xs:complexType>\n"
			      "<xs:element name=\"result\">\n"
			      "  <xs:complexType>\n"
			      "    <xs:sequence>\n"
			      "      <xs:choice minOccurs=\"0\" maxOccurs=\"unbounded\">\n" // 0..N steps
			      "        <xs:element name=\"road_step\" type=\"RoadStep\"/>\n"
			      "        <xs:element name=\"public_transport_step\" type=\"PublicTransportStep\"/>\n"
			      "      </xs:choice>\n"
			      "      <xs:element name=\"cost\" type=\"Cost\" minOccurs=\"0\" maxOccurs=\"unbounded\"/>\n" // 0..N costs
			      "      <xs:element name=\"overview_path\" minOccurs=\"0\" maxOccurs=\"1\">\n" // 0..1
			      "        <xs:complexType>\n"
			      "          <xs:sequence>\n"
			      "            <xs:element name=\"node\" type=\"DbId\" maxOccurs=\"unbounded\"/>\n"
			      "          </xs:sequence>\n"
			      "        </xs:complexType>\n"
			      "      </xs:element>\n"
			      "    </xs:sequence>\n"
			      "  </xs:complexType>\n"
			      "</xs:element>\n"
			      );
    }

    Service::ParameterMap& ResultService::execute()
    {
	output_parameters_.clear();
	Tempus::Result& result = plugin_->result();

	Tempus::Road::Graph& road_graph = plugin_->get_graph().road;

	Tempus::Roadmap& roadmap = result.back();
	xmlNode* root_node = xmlNewNode( /*ns=*/ NULL, (const xmlChar*)"result" );
	xmlNode* overview_path_node = xmlNewNode( /* ns = */ NULL,
					   (const xmlChar*)"overview_path" );
	Roadmap::VertexList::iterator it;
	for ( it = roadmap.overview_path.begin(); it != roadmap.overview_path.end(); it++ )
	{
	    xmlNode *node = xmlNewNode( /* ns = */ NULL, (const xmlChar*)"node" );
	    xmlAttr *attr = xmlNewProp( node,
					(const xmlChar*)"id", 
					(const xmlChar*)(boost::lexical_cast<string>( road_graph[*it].db_id )).c_str() );
	    xmlAddChild( overview_path_node, node );
	}

	Roadmap::StepList::iterator sit;
	for ( sit = roadmap.steps.begin(); sit != roadmap.steps.end(); sit++ )
	{
	    if ( (*sit)->step_type == Roadmap::Step::RoadStep )
	    {
		Roadmap::RoadStep* step = static_cast<Roadmap::RoadStep*>( *sit );
		xmlNode* road_step_node = xmlNewNode( NULL, (const xmlChar*)"road_step" );
		xmlNode* rs_node = xmlNewNode( NULL, (const xmlChar*)"road_section" );
		Tempus::db_id_t rs_id;
		if ( edge_exists(step->road_section, road_graph) )
		    rs_id = road_graph[ step->road_section].db_id;
		else
		    rs_id = 0;
		xmlNewProp( rs_node,
			    (const xmlChar*)"id",
			    (const xmlChar*)(boost::lexical_cast<string>( rs_id )).c_str() );

		Tempus::db_id_t rd_id;
		if ( edge_exists(step->road_direction, road_graph) )
		    rd_id = road_graph[ step->road_direction].db_id;
		else
		    rd_id = 0;
		xmlNode* rd_node = xmlNewNode( NULL, (const xmlChar*)"road_direction" );
		xmlNewProp( rd_node,
			    (const xmlChar*)"id",
			    (const xmlChar*)(boost::lexical_cast<string>( rd_id )).c_str() );
		xmlNode* distance_node = xmlNewNode( NULL, (const xmlChar*)"distance_km" );
		xmlAddChild( distance_node, xmlNewText((const xmlChar*)(boost::lexical_cast<string>( step->distance_km )).c_str()) );
		xmlNode* end_movement_node = xmlNewNode( NULL, (const xmlChar*)"end_movement" );
		xmlAddChild( end_movement_node, xmlNewText((const xmlChar*)(boost::lexical_cast<string>( step->end_movement )).c_str()) );
		
		xmlAddChild( road_step_node, rs_node );
		xmlAddChild( road_step_node, rd_node );
		xmlAddChild( road_step_node, distance_node );
		xmlAddChild( road_step_node, end_movement_node );
		for ( Tempus::Costs::iterator cit = step->costs.begin(); cit != step->costs.end(); cit++ )
		{
		    xmlNode* cost_node = xmlNewNode( NULL, (const xmlChar*)"cost" );
		    xmlNewProp( cost_node,
				(const xmlChar*)"type",
				(const xmlChar*)(boost::lexical_cast<string>( cit->first )).c_str() );
		    xmlNewProp( cost_node,
				(const xmlChar*)"value",
				(const xmlChar*)(boost::lexical_cast<string>( cit->second )).c_str() );
		    xmlAddChild( road_step_node, cost_node );
		}
		xmlAddChild( root_node, road_step_node );
	    }
	    // TODO: add PT steps
	}
	xmlAddChild( root_node, overview_path_node );

	output_parameters_[ "result" ] = root_node;
	return output_parameters_;
    }

    static PreBuildService pre_build_service_;
    static BuildService build_service_;
    static PreProcessService pre_process_service_;
    static ProcessService process_service_;
    static ResultService result_service_;

}; // WPS namespace