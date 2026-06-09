#include <cloudServer/service.hpp>

#include <iostream>
#include <stdexcept>
#include <string>

int main(int argc, char** argv) {
    try {
        CloudServer::ServerConfig config = CloudServer::ServerConfig::fromEnvironment();
        if (auto error = config.validateForServe()) {
            std::cerr << "cloud-server config error: " << *error << "\n";
            return 2;
        }

        CloudServer::RegistryDatabase database(config);
        CloudServer::S3Storage storage(config);

        bool migrate = argc > 1 && std::string(argv[1]) == "--migrate";
        bool migrateAndServe = argc > 1 && std::string(argv[1]) == "--migrate-and-serve";
        if (migrate || migrateAndServe) {
            database.runMigrations();
            std::cout << "cloud-server migrations applied\n";
            if (migrate) {
                return 0;
            }
        }

        CloudServer::RegistryService service(config, database, storage);
        CloudServer::HttpServer server(config, service);
        server.start();
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "cloud-server error: " << error.what() << "\n";
        return 1;
    }
}
