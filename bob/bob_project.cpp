#include "bob_project.hpp"
#include "utilities.hpp"
#include "spdlog/spdlog.h"
#include <fstream>
#include <chrono>
#include <thread>
#include <string>
#include <charconv>

namespace bob
{
    using namespace std::chrono_literals;


    project::project(  const std::string project_name, std::shared_ptr<spdlog::logger> log ) : project_name(project_name), bob_home_directory("/.bob"), project_directory(".")
    {
        this->log = log;
    }

    project::~project( )
    {
    }

    void project::set_project_directory(const std::string path)
    {
        project_directory = path;
    }

    void project::process_build_string(const std::string build_string)
    {
        // When C++20 ranges are available
        // for (const auto word: std::views::split(build_string, " ")) {

        std::stringstream ss(build_string);
        std::string word;
        while (std::getline(ss, word, ' ')) {
            // Identify features, commands, and components
            if (word.front() == '+')
                this->unprocessed_features.insert(word.substr(1));
            else if (word.back() == '!')
                this->commands.insert(word.substr(0, word.size() - 1));
            else
                this->unprocessed_components.insert(word);
        }
    }

    void project::init_project(const std::string build_string)
    {
        process_build_string(build_string);
        init_project();
    }

    void project::init_project()
    {
        output_path  = bob::default_output_directory;
        project_summary_file = "output/" + project_name + "/bob_summary.yaml";
        // previous_summary["components"] = YAML::Node();

        if (fs::exists(project_summary_file))
        {
            project_summary_last_modified = fs::last_write_time(project_summary_file);
            project_summary = YAML::LoadFile(project_summary_file);

            // Fill required_features with features from project summary
            for (const auto& f: project_summary["features"])
                required_features.insert(f.as<std::string>());

            // Remove the known components from unprocessed_components
            component_list_t temp_components;
            for (const auto& c: unprocessed_components)
                if (!project_summary["components"][c])
                    temp_components.insert(c);
            feature_list_t temp_features;
            for (const auto& c: unprocessed_features)
                if (!project_summary["features"][c])
                    temp_features.insert(c);
            unprocessed_components = std::move(temp_components);
            unprocessed_features = std::move(temp_features);

            update_summary();
        }
        else
            fs::create_directories(output_path);
    }

    YAML::Node project::get_project_summary()
    {
        return project_summary;
    }

    void project::process_requirements(YAML::Node& component, YAML::Node child_node)
    {
        // Merge the feature values into the parent component
        yaml_node_merge( component, child_node );

        // If the feature has no requires we stop here
        if ( !child_node["requires"] )
            return;

        // Process all the requires for this feature
        auto child_node_requirements = child_node["requires"];
        if (child_node_requirements.IsScalar () || child_node_requirements.IsSequence ())
        {
            log->error("Node 'requires' entry is malformed: '{}'", child_node_requirements.Scalar());
            return;
        }

        try
        {
            // Process required components
            if (child_node_requirements["components"])
            {
                // Add the item/s to the new_component list
                if (child_node_requirements["components"].IsScalar())
                    unprocessed_components.insert(child_node_requirements["components"].as<std::string>());
                else if (child_node_requirements["components"].IsSequence())
                    for (const auto &i : child_node_requirements["components"])
                        unprocessed_components.insert(i.as<std::string>());
                else
                    log->error("Node '{}' has invalid 'requires'", child_node_requirements.Scalar());
            }

            // Process required features
            if (child_node_requirements["features"])
            {
                std::vector<std::string> new_features;

                // Add the item/s to the new_features list
                if (child_node_requirements["features"].IsScalar())
                    unprocessed_features.insert(child_node_requirements["features"].as<std::string>());
                else if (child_node_requirements["features"].IsSequence())
                    for (const auto &i : child_node_requirements["features"])
                        unprocessed_features.insert(i.as<std::string>());
                else
                    log->error("Node '{}' has invalid 'requires'", child_node_requirements.Scalar());
            }
        }
        catch (YAML::Exception &e)
        {
            log->error("Failed to process requirements for '{}'\n{}", child_node_requirements.Scalar(), e.msg);
        }
    }


    void project::update_summary()
    {
        // Check if any component files have been modified
        for (const auto& c: project_summary["components"])
        {
            const auto name = c.first.as<std::string>();
            if (!c.second["bob_file"]) {
                log->error("Project summary for component '{}' is missing 'bob_file' entry", name);
                project_summary["components"].remove(name);
                unprocessed_components.insert(name);
                continue;
             }

            auto bob_file = c.second["bob_file"].as<std::string>();            

            if ( !fs::exists(bob_file) || fs::last_write_time(bob_file) > project_summary_last_modified)
            {
                // If so, move existing data to previous summary
                previous_summary["components"][name] = std::move(c.second); // TODO: Verify this is correct way to do this efficiently
                project_summary["components"][name] = {};
                unprocessed_components.insert(name);
            }
            else
            {
                // Previous summary should point to the same object
                previous_summary["components"][name] = c.second;

                auto a = previous_summary["components"][name];
                auto b = project_summary["components"][name];
            }
        }
    }

    /**
     * @brief Processes all the @ref unprocessed_components and @ref unprocessed_features, adding items to @ref unknown_components if they are not in the component database
     *        It is assumed the caller will process the @ref unknown_components before adding them back to @ref unprocessed_component and calling this again.
     * @return project::state
     */
    project::state project::evaluate_dependencies()
    {
        // Start processing all the required components and features
        while ( !unprocessed_components.empty( ) || !unprocessed_features.empty( ))
        {
            // Loop through the list of unprocessed components.
            // Note: Items will be added to unprocessed_components during processing
            component_list_t temp_component_list = std::move(unprocessed_components);
            for (const auto& i: temp_component_list)
            {
                // Convert string to id
                const auto c = bob::component_dotname_to_id(i);

                // Find the component in the project component database
                auto component_path = find_component(c);
                if ( !component_path )
                {
                    // log->info("{}: Couldn't find it", c);
                    unknown_components.insert(c);
                    continue;
                }

                // Add component to the required list and continue if this is not a new component
                // Insert component and continue if this is not new 
                if ( required_components.insert( c ).second == false )
                    continue;

                std::shared_ptr<bob::component> new_component = std::make_shared<bob::component>();
                if (!new_component->parse_file( component_path.value(), blueprint_database ).IsNull())
                    components.push_back( new_component );
                else
                    return project::state::PROJECT_HAS_INVALID_COMPONENT;

                // Add all the required components into the unprocessed list
                for (const auto& r : new_component->yaml["requires"]["components"])
                    unprocessed_components.insert(r.Scalar());

                // Add all the required features into the unprocessed list
                for (const auto& f : new_component->yaml["requires"]["features"])
                    unprocessed_features.insert(f.Scalar());

                // Process all the currently required features. Note new feature will be processed in the features pass
                for ( auto& f : required_features )
                    if ( new_component->yaml["supports"]["features"][f] )
                    {
                        log->info("Processing feature '{}' in {}", f, c);
                        process_requirements(new_component->yaml, new_component->yaml["supports"]["features"][f]);
                    }

                // Process the new components support for all the currently required components
                for ( auto& d : required_components )
                    if ( new_component->yaml["supports"]["components"][d] )
                    {
                        log->info("Processing component '{}' in {}", d, c);
                        process_requirements(new_component->yaml, new_component->yaml["supports"]["components"][d]);
                    }
                
                // Process all the existing components support for the new component
                for ( auto& d: components)
                    if (d->yaml["supports"]["components"][c])
                    {
                        log->info("Processing component '{}' in {}", c, d->yaml["name"].Scalar());
                        process_requirements(d->yaml, d->yaml["supports"]["components"][c]);
                    }
            }

            // Process all the new features
            // Note: Items will be added to unprocessed_features during processing
            feature_list_t temp_feature_list = std::move(unprocessed_features);
            for (const auto& f: temp_feature_list)
            {
                // Insert feature and continue if this is not new
                if ( required_features.insert( f ).second == false )
                    continue;

                // Process the feature "supports" for each existing component
                for ( auto& c : components )
                    if ( c->yaml["supports"]["features"][f] )
                    {
                        log->info("Processing feature '{}' in {}", f, c->yaml["name"].Scalar());
                        process_requirements(c->yaml, c->yaml["supports"]["features"][f]);
                    }
            }
        };

        if (unknown_components.size() != 0) return project::state::PROJECT_HAS_UNKNOWN_COMPONENTS;

        return project::state::PROJECT_VALID;
    }

    void project::generate_project_summary()
    {
        // Add standard information into the project summary
        project_summary["project_name"]   = project_name;
        project_summary["project_output"] = default_output_directory + project_name;
        project_summary["configuration"]["host_os"] = host_os_string;
        project_summary["configuration"]["executable_extension"] = executable_extension;

        project_summary_json = project_summary.as<nlohmann::json>();

        if (!project_summary["tools"])
          project_summary["tools"] = YAML::Node();

        // Put all YAML nodes into the summary
        for (const auto& c: components)
        {
            project_summary["components"][c->id] = c->yaml;
            for (auto tool: c->yaml["tools"])
            {
                inja::Environment inja_env = inja::Environment();
                inja_env.add_callback("curdir", 0, [&c](const inja::Arguments& args) { return c->yaml["directory"].Scalar();});

                project_summary["tools"][tool.first.Scalar()] = try_render(inja_env, tool.second.Scalar(), project_summary_json, log);
            }
        }

        project_summary["features"] = {};
        for (const auto& i: this->required_features)
        	project_summary["features"].push_back(i);

        project_summary_json = project_summary.as<nlohmann::json>();
        project_summary_json["data"] = nlohmann::json::object();
        project_summary_json["host"] = nlohmann::json::object();
        project_summary_json["host"]["name"] = host_os_string;
    }

    std::optional<fs::path> project::find_component(const std::string component_dotname)
    {
        const std::string component_id = bob::component_dotname_to_id(component_dotname);

        // Get component from database
        const auto& c = component_database[component_id];

        // Check if that component is in the database
        if (!c)
        {
            // TODO: Check the filesystem 
            return {};
        }

        if (c.IsScalar() && fs::exists(c.Scalar()))
            return c.Scalar();
        if (c.IsSequence())
        {
            if ( c.size( ) == 1 )
            {
                if ( fs::exists( c[0].Scalar( ) ) )
                    return c[0].Scalar( );
            }
            else
                log->error("TODO: Parse multiple matches to the same component ID: '{}'", component_id);
        }
        return {};
    }

    void project::parse_blueprints()
    {
        for ( auto c: project_summary["components"])
            for ( auto b: c.second["blueprints"] )
            {
                std::string blueprint_string = try_render(inja_environment,  b.second["regex"] ? b.second["regex"].Scalar() : b.first.Scalar( ), project_summary_json, log);
                log->info("Blueprint: {}", blueprint_string);
                blueprint_database.blueprints.insert({blueprint_string, std::make_shared<blueprint>(blueprint_string, b.second, c.second["directory"].Scalar())});
            }
    }

    void project::generate_target_database()
    {
        std::vector<std::string> new_targets;
        std::unordered_set<std::string> processed_targets;
        std::vector<std::string> unprocessed_targets;

        for (const auto& c: commands)
            unprocessed_targets.push_back(c);

        while (!unprocessed_targets.empty())
        {
            for (const auto& t: unprocessed_targets)
            {
                // Add to processed targets and check if it's already been processed
                if (processed_targets.insert(t).second == false)
                    continue;

                // Do not add to task database if it's a data dependency. There is special processing of these.
                if (t.front() == data_dependency_identifier)
                    continue;

                // Check if target is not in the database. Note task_database is a multimap
                if (target_database.find(t) == target_database.end())
                {
                    add_to_target_database(t);
                }
                auto tasks = target_database.equal_range(t);

                std::for_each(tasks.first, tasks.second, [&new_targets](auto& i) {
                    new_targets.insert(new_targets.end(), i.second->dependencies.begin(), i.second->dependencies.end());
                });
            }

            unprocessed_targets.clear();
            unprocessed_targets.swap(new_targets);
        }
    }

    bool project::has_data_dependency_changed(std::string data_path)
    {
        std::vector<std::pair<const YAML::Node&, const YAML::Node&>> changed_nodes;

        if (data_path.front() != data_dependency_identifier)
            return false;

        std::lock_guard<std::mutex> lock(project_lock);
        try
        {
            if (previous_summary.IsNull() || previous_summary["components"].IsNull())
                return true;
            
            // Check for wildcard or component name
            if (data_path[1] == data_wildcard_identifier)
            {
                if (data_path[2] != '.')
                {
                    log->error("Data dependency malformed: {}", data_path);
                    return false;
                }

                for (const auto& i: project_summary["components"])
                {
                    std::string component_name = i.first.as<std::string>();
                    if (!(previous_summary["components"][component_name] == project_summary["components"][component_name] ))
                        changed_nodes.push_back({project_summary["components"][component_name], previous_summary["components"][component_name]});
                }
                data_path = data_path.substr(3);
            }
            else
            {
                std::string component_name = data_path.substr(1, data_path.find_first_of('.')-1);
                data_path = data_path.substr(data_path.find_first_of('.')+1);
                if (!(previous_summary["components"][component_name] == project_summary["components"][component_name] ))
                {
                    changed_nodes.push_back({project_summary["components"][component_name], previous_summary["components"][component_name]});
                }
            }

            // Check if we have any nodes that have changed
            if (changed_nodes.size() == 0)
                return false;

            // Check every data path for every changed node
            for (const auto& n: changed_nodes)
            {
                auto first = yaml_path(n.first, data_path);
                auto second = yaml_path(n.second, data_path);
                if (yaml_diff(first, second))
                    return true;
            }
            return false;
        }
        catch(const std::exception& e)
        {
            log->error("Failed to determine data dependency: {}", e.what());
            return false;
        }
    }

    void project::add_to_target_database( const std::string target )
    {
        bool blueprint_match_found = false;

        for ( const auto& blueprint : blueprint_database.blueprints )
        {
            auto match = std::make_shared<blueprint_match>();

            // Check if rule is a regex, otherwise do a string comparison
            if ( blueprint.second->regex.has_value() )
            {
                std::smatch s;
                if (!std::regex_match(target, s, std::regex { blueprint.first } ) )
                    continue;

                // arg_count starts at 0 as the first match is the entire string
                for ( auto& regex_match : s )
                    match->regex_matches.push_back(regex_match.str( ));
            }
            else
            {
                if (target != blueprint.first )
                    continue;

                match->regex_matches.push_back(target);
            }

            // Found a match. Create a blueprint match object
            blueprint_match_found = true;
            match->blueprint = blueprint.second;

            inja::Environment local_inja_env;
            local_inja_env.add_callback("$", 1, [&match](const inja::Arguments& args) { return match->regex_matches[ args[0]->get<int>() ];});
            local_inja_env.add_callback("curdir", 0, [&match](const inja::Arguments& args) { return match->blueprint->parent_path;});
            local_inja_env.add_callback("notdir", 1, [](inja::Arguments& args) { return std::filesystem::path{args.at(0)->get<std::string>()}.filename();});
            local_inja_env.add_callback("absolute_dir", 1, [](inja::Arguments& args) { return std::filesystem::absolute(args.at(0)->get<std::string>());});
            local_inja_env.add_callback("extension", 1, [](inja::Arguments& args) { return std::filesystem::absolute(args.at(0)->get<std::string>());});
            local_inja_env.add_callback("render", 1, [&](const inja::Arguments& args) { return local_inja_env.render(args[0]->get<std::string>(), this->project_summary_json);});
            local_inja_env.add_callback("read_file", 1, [&](const inja::Arguments& args) { 
                auto file = std::ifstream(args[0]->get<std::string>()); 
                return std::string{std::istreambuf_iterator<char>{file}, {}};
            });
            local_inja_env.add_callback("aggregate", 1, [&](const inja::Arguments& args) {
                YAML::Node aggregate;
                const std::string path = args[0]->get<std::string>();
                // Loop through components, check if object path exists, if so add it to the aggregate
                for (const auto& c: this->project_summary["components"])
                {
                    auto v = yaml_path(c.second, path);
                    if (!v)
                        continue;
                    
                    if (v.IsMap())
                        for (auto i: v)
                            aggregate[i.first] = i.second; //local_inja_env.render(i.second.as<std::string>(), this->project_summary_json);
                    else if (v.IsSequence())
                        for (auto i: v)
                            aggregate.push_back(local_inja_env.render(i.as<std::string>(), this->project_summary_json));
                    else
                        aggregate.push_back(local_inja_env.render(v.as<std::string>(), this->project_summary_json));
                }
                if (aggregate.IsNull())
                    return nlohmann::json();
                else
                    return aggregate.as<nlohmann::json>();
                });

            // Run template engine on dependencies
            for ( auto d : blueprint.second->dependencies )
            {
                switch (d.type)
                {
                    case blueprint::dependency::DEPENDENCY_FILE_DEPENDENCY:
                    {
                        const std::string generated_dependency_file = try_render(local_inja_env,  d.name, project_summary_json, log );
                        auto dependencies = parse_gcc_dependency_file(generated_dependency_file);
                        match->dependencies.insert( std::end( match->dependencies ), std::begin( dependencies ), std::end( dependencies ) );
                        continue;
                    }
                    case blueprint::dependency::DATA_DEPENDENCY:
                    {
                        std::string data_name = try_render(local_inja_env, d.name, project_summary_json, log);
                        if (data_name.front() != data_dependency_identifier)
                            data_name.insert(0,1, data_dependency_identifier);
                        match->dependencies.push_back(data_name);
                        continue;
                    }
                    default:
                        break;
                }

                // Generate full dependency string by applying template engine
                std::string generated_depend;
                try
                {
                    generated_depend = local_inja_env.render( d.name, project_summary_json );
                }
                catch ( std::exception& e )
                {
                    log->error("Couldn't apply template: '{}'", d.name);
                    return;
                }

                // Check if the input was a YAML array construct
                if ( generated_depend.front( ) == '[' && generated_depend.back( ) == ']' )
                {
                    // Load the generated dependency string as YAML and push each item individually
                    try {
                        auto generated_node = YAML::Load( generated_depend );
                        for ( auto i : generated_node ) {
                            auto temp = i.Scalar();
                            match->dependencies.push_back( temp.starts_with("./") ? temp.substr(2) : temp );
                        }
                    } catch ( std::exception& e ) {
                        std::cerr << "Failed to parse dependency: " << d.name << "\n";
                    }
                }
                else
                {
                    match->dependencies.push_back( generated_depend.starts_with("./") ? generated_depend.substr(2) : generated_depend );
                }
            }

            target_database.insert(std::make_pair(target, match));
        }

        if (!blueprint_match_found)
        {
            if (!fs::exists( target ))
                log->info("No blueprint for '{}'", target);
            // task_database.insert(std::make_pair(target, std::make_shared<blueprint_node>(target)));
        }
    }

    void project::load_common_commands()
    {
        blueprint_commands["echo"] = []( std::string target, const YAML::Node& command, std::string captured_output, const nlohmann::json& generated_json, inja::Environment& inja_env) -> std::string {
            auto console = spdlog::get("bobconsole");
            auto boblog = spdlog::get("boblog");
            if (!command.begin()->second.IsNull())
                captured_output = try_render(inja_env, command.begin()->second.as<std::string>(), generated_json, boblog);

            console->info("{}", captured_output);
            return captured_output;
        };


        blueprint_commands["execute"] = []( std::string target, const YAML::Node& command, std::string captured_output, const nlohmann::json& generated_json, inja::Environment& inja_env) -> std::string {
            if (command.begin()->second.IsNull())
                return "";
            auto boblog = spdlog::get("boblog");
            auto console = spdlog::get("bobconsole");
            std::string temp = command.begin( )->second.as<std::string>( );
            try {
                captured_output = inja_env.render( temp, generated_json );
                //std::replace( captured_output.begin( ), captured_output.end( ), '/', '\\' );
                boblog->info("Executing '{}'", captured_output);
                auto [temp_output, retcode] = exec( captured_output, std::string( "" ) );

                if (retcode != 0 && temp_output.length( ) != 0) {
                    console->error( temp_output );
                    boblog->error( "\n{} returned {}\n{}", captured_output, retcode, temp_output);
                }
                else if ( temp_output.length( ) != 0 )
                    boblog->info("{}", temp_output);
                return temp_output;
            }
            catch (std::exception& e)
            {
                boblog->error( "Failed to execute: {}\n{}", temp, e.what());
                captured_output = "";
                return "";
            }
        };

        blueprint_commands["fix_slashes"] = []( std::string target, const YAML::Node& command, std::string captured_output, const nlohmann::json& generated_json, inja::Environment& inja_env) -> std::string {
            std::replace(captured_output.begin(), captured_output.end(), '\\', '/');
            return captured_output;
        };


        blueprint_commands["regex"] = []( std::string target, const YAML::Node& command, std::string captured_output, const nlohmann::json& generated_json, inja::Environment& inja_env) -> std::string {
            std::regex regex_search(command["search"].as<std::string>());
            auto boblog = spdlog::get("boblog");
            if (command["split"])
            {
                std::istringstream ss(captured_output);
                std::string line;
                captured_output = "";

                while (std::getline(ss, line))
                {
                  if (command["replace"])
                  {
                      std::string r = std::regex_replace(line, regex_search, command["replace"].as<std::string>(), std::regex_constants::format_no_copy);
                      captured_output.append(r);
                  }
                  else if (command["to_yaml"])
                  {
                    std::smatch s;
                    if (!std::regex_match(line, s, regex_search))
                      continue;
                    YAML::Node node;
                    node[0] = YAML::Node();
                    int i=1;
                    for ( auto& v : command["to_yaml"] )
                      node[0][v.Scalar()] = s[i++].str();

                    captured_output.append(YAML::Dump(node));
                    captured_output.append("\n");
                  }
                }
            }
            else if (command["replace"])
            {
                captured_output = std::regex_replace(captured_output, regex_search, command["replace"].as<std::string>());
            }
            else
            {
                boblog->error("'regex' command does not have enough information");
            }
            return captured_output;
        };


        blueprint_commands["inja"] = []( std::string target, const YAML::Node& command, std::string captured_output, const nlohmann::json& generated_json, inja::Environment& inja_env) -> std::string {
            auto boblog = spdlog::get("boblog");
            try
            {
                std::string template_string;
                std::string template_filename;
                nlohmann::json data;
                if (command["template_file"])
                    template_filename = try_render(inja_env, command["template_file"].as<std::string>(), generated_json, boblog);
                else
                    template_string = (command["template"]) ? command["template"].Scalar() : command["inja"].Scalar();

                if (command["data_file"])
                {
                    std::string data_filename = try_render(inja_env, command["data_file"].as<std::string>(), generated_json, boblog);
                    YAML::Node data_yaml = YAML::LoadFile(data_filename);
                    data = data_yaml.as<nlohmann::json>();
                }

                if (template_filename.empty())
                    captured_output = try_render(inja_env, template_string, data.is_null() ? generated_json : data, boblog);
                else
                    captured_output = inja_env.render_file(template_filename, data.is_null() ? generated_json : data);
            }
            catch (std::exception &e)
            {
                boblog->error("Failed to apply template: {}\n{}", command.Scalar(), e.what());
            }
            return captured_output;
        };

        blueprint_commands["save"] = []( std::string target, const YAML::Node& command, std::string captured_output, const nlohmann::json& generated_json, inja::Environment& inja_env) -> std::string {
            std::string save_filename;
            auto boblog = spdlog::get("boblog");

            if (command.begin()->second.IsNull())
                save_filename = target;
            else
                save_filename = try_render(inja_env, command.begin()->second.as<std::string>(), generated_json, boblog);

            try
            {
                std::ofstream save_file;
                fs::path p(save_filename);
                if (!p.parent_path().empty())
                  fs::create_directories(p.parent_path());
                save_file.open(save_filename, std::ios_base::binary);
                save_file << captured_output;
                save_file.close();
            }
            catch (std::exception& e)
            {
                boblog->error("Failed to save file: '{}'", save_filename);
                captured_output = "";
            }
            return captured_output;
        };

        blueprint_commands["create_directory"] = [ ]( std::string target, const YAML::Node& command, std::string captured_output, const nlohmann::json& generated_json, inja::Environment& inja_env ) -> std::string {
            auto boblog = spdlog::get("boblog");
            if (!command.begin()->second.IsNull())
            {
                std::string filename = "";
                try
                {
                    filename = command.begin()->second.as<std::string>( );
                    filename = try_render(inja_env, filename, generated_json, boblog);
                    if ( !filename.empty( ) )
                    {
                        fs::path p( filename );
                        fs::create_directories( p.parent_path( ) );
                    }
                }
                catch ( std::exception e )
                {
                    boblog->error( "Couldn't create directory for '{}'", filename);
                }
            }
            return "";
        };

        blueprint_commands["verify"] = [ ]( std::string target, const YAML::Node& command, std::string captured_output, const nlohmann::json& generated_json, inja::Environment& inja_env ) -> std::string {
            auto boblog = spdlog::get("boblog");
            std::string filename = command.begin()->second.as<std::string>( );
            filename = try_render(inja_env, filename, generated_json, boblog);
            if (fs::exists(filename))
                boblog->info("{} exists", filename);
            else
                boblog->info("BAD!! {} doesn't exist", filename);
            return captured_output;
        };

        blueprint_commands["rm"] = [ ]( std::string target, const YAML::Node& command, std::string captured_output, const nlohmann::json& generated_json, inja::Environment& inja_env ) -> std::string {
            auto boblog = spdlog::get("boblog");
            std::string filename = command.begin()->second.as<std::string>( );
            filename = try_render(inja_env, filename, generated_json, boblog);
            fs::remove(filename);
            return captured_output;
        };

        blueprint_commands["pack"] = [ ]( std::string target, const YAML::Node& command, std::string captured_output, const nlohmann::json& generated_json, inja::Environment& inja_env ) -> std::string {
            auto boblog = spdlog::get("boblog");
            std::vector<std::byte> data_output;

            if (!command["data"])
            {
                boblog->error("'pack' command requires 'data'\n");
                return captured_output;
            } else if (!command["format"])
            {
                boblog->error("'pack' command requires 'format'\n");
                return captured_output;
            }

            std::string format = command["format"].as<std::string>();
            format = try_render(inja_env, format, generated_json, boblog);
            
            auto i = format.begin();
            for (auto d: command["data"])
            {
                auto v = try_render(inja_env, d.as<std::string>(), generated_json, boblog);
                const char c = *i++;
                union {
                    int8_t s8;
                    uint8_t u8;
                    int16_t  s16;
                    uint16_t u16;
                    int32_t s32;
                    uint32_t u32;
                    unsigned long value;
                    std::byte bytes[8];
                } temp;
                const auto result = (v.size() > 1 && v[1] == 'x') ? std::from_chars(v.data()+2, v.data() + v.size(), temp.u32, 16) : 
                                    (v[0] == '-') ? std::from_chars(v.data(), v.data() + v.size(), temp.s32) : std::from_chars(v.data(), v.data() + v.size(), temp.u32);
                if (result.ec != std::errc())
                {
                    boblog->error("Error converting number: {}\n", v);
                }
                
                switch(c) {
                    case 'L': data_output.insert(data_output.end(), &temp.bytes[0], &temp.bytes[4]); break;
                    case 'l': data_output.insert(data_output.end(), &temp.bytes[0], &temp.bytes[4]); break;
                    case 'S': data_output.insert(data_output.end(), &temp.bytes[0], &temp.bytes[2]); break;
                    case 's': data_output.insert(data_output.end(), &temp.bytes[0], &temp.bytes[2]); break;
                    case 'C': data_output.insert(data_output.end(), &temp.bytes[0], &temp.bytes[1]); break;
                    case 'c': data_output.insert(data_output.end(), &temp.bytes[0], &temp.bytes[1]); break;
                    case 'x': data_output.push_back(std::byte{0}); break;
                    default: boblog->error("Unknown pack type\n"); break;
                }
            }
            auto chars = reinterpret_cast<char const*>(data_output.data());
            captured_output.insert(captured_output.end(), chars, chars + data_output.size());
            return captured_output;
        };

        blueprint_commands["copy"] = [ ]( std::string target, const YAML::Node& command, std::string captured_output, const nlohmann::json& generated_json, inja::Environment& inja_env ) -> std::string {
            auto boblog = spdlog::get("boblog");
            try 
            {
                std::string source      = try_render(inja_env, command["source"].as<std::string>( ), generated_json, boblog);
                std::string destination = try_render(inja_env, command["destination"].as<std::string>( ), generated_json, boblog);
                std::filesystem::copy(source, destination, std::filesystem::copy_options::recursive);
            }
            catch (std::exception& e)
            {
                boblog->error("'copy' command failed while processing {}", target);
            }
            return "";
        };

        blueprint_commands["cat"] = [ ]( std::string target, const YAML::Node& command, std::string captured_output, const nlohmann::json& generated_json, inja::Environment& inja_env ) -> std::string {
            auto boblog = spdlog::get("boblog");
            std::string filename = try_render(inja_env, command.begin()->second.as<std::string>( ), generated_json, boblog);
            std::ifstream datafile;
            datafile.open(filename, std::ios_base::in | std::ios_base::binary);
            std::string line;
            while (std::getline(datafile, line))
                captured_output.append(line);
            datafile.close();
            return captured_output;
        };
    }

    void project::create_tasks(const std::string target_name, tf::Task& parent)
    {
        // XXX: Start time should be determined at the start of the executable and not here
        auto start_time = fs::file_time_type::clock::now();

        // Check if this target has already been processed
        const auto& existing_todo = todo_list.equal_range(target_name);
        if (existing_todo.first != existing_todo.second)
        {
            // Add parent to the dependency graph
            for (auto i=existing_todo.first; i != existing_todo.second; ++i)
                i->second.task.precede(parent);

            // Nothing else to do
            return;
        }

        // Get targets that match the name
        const auto& targets = target_database.equal_range(target_name);

        // If there is no targets then it must be a leaf node (source file, data dependency, etc)
        if (targets.first == targets.second)
        {
            auto new_todo = todo_list.insert(std::make_pair(target_name, construction_task()));
            auto task = taskflow.placeholder();

            // Check if target is a data dependency
            if (target_name.front() == data_dependency_identifier)
            {
                task.data(&new_todo->second).work([=, this]() {
                    // log->info("{}: data", target_name);
                    auto d = static_cast<construction_task*>(task.data());
                    d->last_modified = has_data_dependency_changed(target_name) ? fs::file_time_type::max() : fs::file_time_type::min();
                    if (d->last_modified > start_time)
                        log->info("{} has been updated", target_name);
                });
            }
            // Check if target name matches an existing file in filesystem
            else if (fs::exists(target_name))
            {
                // Create a new task to retrieve the file timestamp
                task.data(&new_todo->second).work([=]() {
                    auto d = static_cast<construction_task*>(task.data());
                    d->last_modified = fs::last_write_time(target_name);
                    // log->info("{}: timestamp {}", target_name, d->last_modified.time_since_epoch().count());
                });
                
            }
            else
            {
                log->info("Target {} has no action", target_name);
            }
            new_todo->second.task = task;
            new_todo->second.task.precede(parent);
            return;
        }

        for (auto i=targets.first; i != targets.second; ++i)
        {
            ++work_task_count;
            auto new_todo = todo_list.insert(std::make_pair(target_name, construction_task()));
            new_todo->second.match = i->second;
            auto task = taskflow.placeholder();
            task.data(&new_todo->second).work([=,this]() {
                // log->info("{}: process", target_name);
                auto d = static_cast<construction_task*>(task.data());
                if (d->last_modified != fs::file_time_type::min())
                {
                    // I don't think this event happens. This check can probably be removed
                    log->info("{} already done", target_name);
                    return;
                }
                if (d->match)
                {
                    if (fs::exists(target_name))
                        d->last_modified = fs::last_write_time(target_name);

                    // Check if there are no dependencies
                    if (d->match->dependencies.size() == 0)
                    {
                        // If it doesn't exist as a file, run the command
                        if (!fs::exists(target_name))
                        {
                            run_command(i->first, d, this);
                            d->last_modified = fs::file_time_type::clock::now();
                        }
                    }
                    else if (!d->match->blueprint->process.IsNull())
                    {
                        auto max_element = todo_list.end();
                        for ( auto j: d->match->dependencies)
                        {
                            auto temp = todo_list.equal_range(j);
                            auto temp_element = std::max_element(temp.first, temp.second, [](auto const& i, auto const& j) { return i.second.last_modified < j.second.last_modified;});
                            if (max_element == todo_list.end() || temp_element->second.last_modified > max_element->second.last_modified)
                                max_element = temp_element;
                        }
                        if (!fs::exists(target_name) || max_element->second.last_modified > d->last_modified)
                        {
                            log->info("{}: Updating because of {}",target_name, max_element->first);
                            run_command(i->first, d, this);
                            d->last_modified = fs::file_time_type::clock::now();
                        }
                    }
                    else
                    {
                        //log->info("{} has no process", target_name);
                    }
                }
                if (task_complete_handler)
                        task_complete_handler();
            });

            new_todo->second.task = task;
            new_todo->second.task.precede(parent);

            // For each dependency described in blueprint, retrieve or create task, add relationship, and add item to todo list 
            for (auto& dep_target: i->second->dependencies)
                create_tasks(dep_target.starts_with("./") ? dep_target.substr(2) : dep_target, new_todo->second.task);
        }
    }


    /**
     * @brief Save to disk the content of the @ref project_summary to bob_summary.yaml and bob_summary.json
     *
     */
    void project::save_summary()
    {
        if (!fs::exists(project_summary["project_output"].Scalar()))
            fs::create_directories(project_summary["project_output"].Scalar());

        std::ofstream summary_file( project_summary["project_output"].Scalar() + "/bob_summary.yaml" );
        summary_file << project_summary;
        summary_file.close();
        std::ofstream json_file( project_summary["project_output"].Scalar() + "/bob_summary.json" );
		json_file << project_summary_json.dump(3);
		json_file.close();
    }

} /* namespace bob */
