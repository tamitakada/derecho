#include <derecho/conf/conf.hpp>

#include <nlohmann/json.hpp>

#include <cstdlib>
#include <stdexcept>
#include <sys/stat.h>
#include <unistd.h>
#include <stdexcept>
#include <sstream>

namespace derecho {

const std::vector<std::string> Conf::subgroupProfileFields = {
        "max_payload_size",
        "max_reply_payload_size",
        "max_smc_payload_size",
        "block_size",
        "window_size",
        "rdmc_send_algorithm"};

std::unique_ptr<Conf> Conf::singleton = nullptr;

std::atomic<uint32_t> Conf::singleton_initialized_flag = 0;
#define CONF_UNINITIALIZED (0)
#define CONF_INITIALIZING (1)
#define CONF_INITIALIZED (2)

#define MAKE_LONG_OPT_ENTRY(x) \
    { x, required_argument, 0, 0 }
struct option Conf::long_options[] = {
        // [DERECHO]
        MAKE_LONG_OPT_ENTRY(DERECHO_CONTACT_IP),
        MAKE_LONG_OPT_ENTRY(DERECHO_CONTACT_PORT),
        MAKE_LONG_OPT_ENTRY(DERECHO_RESTART_LEADERS),
        MAKE_LONG_OPT_ENTRY(DERECHO_RESTART_LEADER_PORTS),
        MAKE_LONG_OPT_ENTRY(DERECHO_LOCAL_ID),
        MAKE_LONG_OPT_ENTRY(DERECHO_LOCAL_IP),
        MAKE_LONG_OPT_ENTRY(DERECHO_GMS_PORT),
        MAKE_LONG_OPT_ENTRY(DERECHO_STATE_TRANSFER_PORT),
        MAKE_LONG_OPT_ENTRY(DERECHO_SST_PORT),
        MAKE_LONG_OPT_ENTRY(DERECHO_RDMC_PORT),
        MAKE_LONG_OPT_ENTRY(DERECHO_EXTERNAL_PORT),
        MAKE_LONG_OPT_ENTRY(DERECHO_P2P_LOOP_BUSY_WAIT_BEFORE_SLEEP_MS),
        MAKE_LONG_OPT_ENTRY(DERECHO_HEARTBEAT_MS),
        MAKE_LONG_OPT_ENTRY(DERECHO_SST_POLL_CQ_TIMEOUT_MS),
        MAKE_LONG_OPT_ENTRY(DERECHO_RESTART_TIMEOUT_MS),
        MAKE_LONG_OPT_ENTRY(DERECHO_ENABLE_BACKUP_RESTART_LEADERS),
        MAKE_LONG_OPT_ENTRY(DERECHO_DISABLE_PARTITIONING_SAFETY),
        MAKE_LONG_OPT_ENTRY(DERECHO_MAX_P2P_REQUEST_PAYLOAD_SIZE),
        MAKE_LONG_OPT_ENTRY(DERECHO_MAX_P2P_REPLY_PAYLOAD_SIZE),
        MAKE_LONG_OPT_ENTRY(DERECHO_P2P_WINDOW_SIZE),
        MAKE_LONG_OPT_ENTRY(DERECHO_MAX_NODE_ID),
        MAKE_LONG_OPT_ENTRY(LAYOUT_JSON_LAYOUT),
        MAKE_LONG_OPT_ENTRY(LAYOUT_JSON_LAYOUT_FILE),
        // [SUBGROUP/<subgroup name>]
        MAKE_LONG_OPT_ENTRY(SUBGROUP_DEFAULT_RDMC_SEND_ALGORITHM),
        MAKE_LONG_OPT_ENTRY(SUBGROUP_DEFAULT_MAX_PAYLOAD_SIZE),
        MAKE_LONG_OPT_ENTRY(SUBGROUP_DEFAULT_MAX_REPLY_PAYLOAD_SIZE),
        MAKE_LONG_OPT_ENTRY(SUBGROUP_DEFAULT_MAX_SMC_PAYLOAD_SIZE),
        MAKE_LONG_OPT_ENTRY(SUBGROUP_DEFAULT_BLOCK_SIZE),
        MAKE_LONG_OPT_ENTRY(SUBGROUP_DEFAULT_WINDOW_SIZE),
        // [RDMA]
        MAKE_LONG_OPT_ENTRY(RDMA_PROVIDER),
        MAKE_LONG_OPT_ENTRY(RDMA_DOMAIN),
        MAKE_LONG_OPT_ENTRY(RDMA_TX_DEPTH),
        MAKE_LONG_OPT_ENTRY(RDMA_RX_DEPTH),
        // [PERS]
        MAKE_LONG_OPT_ENTRY(PERS_FILE_PATH),
        MAKE_LONG_OPT_ENTRY(PERS_RAMDISK_PATH),
        MAKE_LONG_OPT_ENTRY(PERS_RESET),
        MAKE_LONG_OPT_ENTRY(PERS_MAX_LOG_ENTRY),
        MAKE_LONG_OPT_ENTRY(PERS_MAX_DATA_SIZE),
        MAKE_LONG_OPT_ENTRY(PERS_PRIVATE_KEY_FILE),
        // [INFOSST]
        MAKE_LONG_OPT_ENTRY(CONF_INFO_SST_LOAD_INFO_MULTICAST_RATE),
        MAKE_LONG_OPT_ENTRY(CONF_INFO_SST_CACHE_INFO_MULTICAST_RATE),
        // [LOGGER]
        MAKE_LONG_OPT_ENTRY(LOGGER_LOG_FILE_DEPTH),
        MAKE_LONG_OPT_ENTRY(LOGGER_LOG_TO_TERMINAL),
        MAKE_LONG_OPT_ENTRY(LOGGER_DEFAULT_LOG_LEVEL),
        MAKE_LONG_OPT_ENTRY(LOGGER_SST_LOG_LEVEL),
        MAKE_LONG_OPT_ENTRY(LOGGER_RPC_LOG_LEVEL),
        MAKE_LONG_OPT_ENTRY(LOGGER_VIEWMANAGER_LOG_LEVEL),
        MAKE_LONG_OPT_ENTRY(LOGGER_PERSISTENCE_LOG_LEVEL),
        {0, 0, 0, 0}};

void Conf::initialize(int argc, char* argv[], const char* conf_file, const char* node_conf_file) {
    uint32_t expected = CONF_UNINITIALIZED;
    // if not initialized(0), set the flag to under initialization ...
    if(Conf::singleton_initialized_flag.compare_exchange_strong(
               expected, CONF_INITIALIZING, std::memory_order_acq_rel)) {
        // 1 - determine configuration file path, if possible, or leave real_conf_file "empty"
        std::optional<std::string> real_conf_file;
        struct stat buffer;
        if(conf_file)
            real_conf_file = conf_file;
        else if(std::getenv("DERECHO_CONF_FILE"))
            // try environment variable: DERECHO_CONF_FILE
            real_conf_file = std::getenv("DERECHO_CONF_FILE");
        else if(stat(default_conf_file, &buffer) == 0) {
            if(S_ISREG(buffer.st_mode) && (S_IRUSR | buffer.st_mode)) {
                real_conf_file = default_conf_file;
            }
        }
        // 1.5 - same path detection but for node_conf_file
        std::optional<std::string> real_node_conf_file;
        if(node_conf_file) {
            real_node_conf_file = node_conf_file;
        } else if(std::getenv("DERECHO_NODE_CONF_FILE")) {
            real_node_conf_file = std::getenv("DERECHO_NODE_CONF_FILE");
        } else if(stat(default_node_conf_file, &buffer) == 0) {
            if(S_ISREG(buffer.st_mode) && (S_IRUSR | buffer.st_mode)) {
                real_node_conf_file = default_node_conf_file;
            }
        }

        // 2 - load configuration
        Conf::singleton = std::make_unique<Conf>(argc, argv, real_conf_file, real_node_conf_file);

        // 3 - set optional log-level keys to equal the default log level if they are not present
        const std::string& default_log_level = Conf::singleton->getString(LOGGER_DEFAULT_LOG_LEVEL);
        Conf::singleton->config.try_emplace(LOGGER_SST_LOG_LEVEL, default_log_level);
        Conf::singleton->config.try_emplace(LOGGER_RPC_LOG_LEVEL, default_log_level);
        Conf::singleton->config.try_emplace(LOGGER_VIEWMANAGER_LOG_LEVEL, default_log_level);
        Conf::singleton->config.try_emplace(LOGGER_PERSISTENCE_LOG_LEVEL, default_log_level);

        // 4 - set the flag to initialized
        Conf::singleton_initialized_flag.store(CONF_INITIALIZED, std::memory_order_acq_rel);

        // 5 - check the configuration for sanity
        if(hasCustomizedConfKey(LAYOUT_JSON_LAYOUT) && hasCustomizedConfKey(LAYOUT_JSON_LAYOUT_FILE)) {
            throw std::logic_error("Configuration error: Both json_layout and json_layout_file were specified. These options are mutually exclusive");
        }
        if(hasCustomizedConfKey(LAYOUT_JSON_LAYOUT_FILE)) {
            std::ifstream json_file_stream(getConfString(LAYOUT_JSON_LAYOUT_FILE));
            if(!json_file_stream) {
                throw std::logic_error("Configuration error: The JSON layout file could not be opened for reading");
            }
            nlohmann::json json_obj;
            try {
                json_file_stream >> json_obj;
            } catch(nlohmann::json::exception& ex) {
                //Wrap the JSON-specific exception in a logic_error to add a message
                std::throw_with_nested(std::logic_error("Configuration error: The JSON layout file does not contain valid JSON"));
            }
        }
        if(hasCustomizedConfKey(LAYOUT_JSON_LAYOUT)) {
            nlohmann::json json_obj;
            try {
                json_obj = nlohmann::json::parse(getConfString(LAYOUT_JSON_LAYOUT));
            } catch(nlohmann::json::exception& ex) {
                std::throw_with_nested(std::logic_error("Configuration error: The JSON layout string is not valid JSON"));
            }
        }

        if(getConfUInt32(DERECHO_LOCAL_ID) >= getConfUInt32(DERECHO_MAX_NODE_ID)) {
            throw std::logic_error("Configuration error: Local node ID must be less than max node ID");
        }
        if(getConfUInt32(SUBGROUP_DEFAULT_MAX_REPLY_PAYLOAD_SIZE) < DERECHO_MIN_RPC_RESPONSE_SIZE) {
            throw std::logic_error(std::string("Configuration error: Default subgroup reply size must be at least ")
                                   + std::to_string(DERECHO_MIN_RPC_RESPONSE_SIZE));
        }
        if(getConfUInt32(DERECHO_MAX_P2P_REPLY_PAYLOAD_SIZE) < DERECHO_MIN_RPC_RESPONSE_SIZE) {
            throw std::logic_error(std::string("Configuration error: P2P reply payload size must be at least ")
                                   + std::to_string(DERECHO_MIN_RPC_RESPONSE_SIZE));
        }
    }
}

void Conf::loadFromFile(const std::string& file_name) {
    getpot::GetPot file_parser(file_name);
    for(const std::string& key : file_parser.get_variable_names()) {
        this->config[key] = file_parser(key, "");
    }
}

Conf::Conf(int argc, char* argv[], std::optional<std::string> group_conf_file,
         std::optional<std::string> node_conf_file) noexcept {
    // 1 - load configuration from configuration file
    if(group_conf_file) {
        loadFromFile(group_conf_file.value());
    }
    if(node_conf_file) {
        loadFromFile(node_conf_file.value());
    }
    // 2 - load configuration from the command line
    bool command_options_present = false;
    int c;
    while(1) {
        int option_index = 0;

        c = getopt_long(argc, argv, "", long_options, &option_index);
        if(c == -1) {
            break;
        }

        switch(c) {
            case 0:
                this->config[long_options[option_index].name] = optarg;
                command_options_present = true;
                break;

            case '?':
                break;

            default:
                std::cerr << "ignore unknown commandline code:" << c << std::endl;
        }
    }
    // 3 - warn the user if no options were loaded, since this probably indicates an error
    if(!group_conf_file && !node_conf_file && !command_options_present) {
        std::cerr << "Warning: derecho.cfg and derecho_node.cfg not found, and no command-line options specified. Falling back to all default configuration options." << std::endl;
    }
}

// should we force the user to call Conf::initialize() by throw an expcetion
// for uninitialized configuration?
const Conf* Conf::get() noexcept(true) {
    while(Conf::singleton_initialized_flag.load(std::memory_order_acquire) != CONF_INITIALIZED) {
        char* empty_arg[1] = {nullptr};
        Conf::initialize(0, empty_arg, nullptr);
    }
    return Conf::singleton.get();
}

void Conf::loadExtraFile(const std::string& default_file_name, const char* env_var_name) {
    std::string real_file_name;
    // Use the file named in the environment variable if it exists and is readable
    if(env_var_name) {
        const char* env_filename = std::getenv(env_var_name);
        struct stat stat_buffer;
        if(env_filename && stat(env_filename, &stat_buffer) == 0) {
            if(S_ISREG(stat_buffer.st_mode) && (S_IRUSR | stat_buffer.st_mode)) {
                real_file_name = env_filename;
            }
        }
    }
    // If that didn't work, see if the default file name works
    if(real_file_name.empty()) {
        struct stat stat_buffer;
        if(stat(default_file_name.c_str(), &stat_buffer) == 0) {
            if(S_ISREG(stat_buffer.st_mode) && (S_IRUSR | stat_buffer.st_mode)) {
                real_file_name = default_file_name;
            }
        }
    }
    // If both the environment variable and the default don't exist, throw an error
    if(real_file_name.empty()) {
        throw std::runtime_error("Could not open configuration file " + default_file_name);
    }
    while(Conf::singleton_initialized_flag.load(std::memory_order_acquire) != CONF_INITIALIZED) {
        char* empty_arg[1] = {nullptr};
        Conf::initialize(0, empty_arg, nullptr);
    }
    singleton->loadFromFile(real_file_name);
}

std::string Conf::getDebugString() const {
    std::stringstream debug_str;
    for(const auto& kv_pair : config) {
        debug_str << "\t" << kv_pair.first << " = " << kv_pair.second << std::endl;
    }
    return debug_str.str();
}

const std::string& getConfString(const std::string& key) {
    return Conf::get()->getString(key);
}

const int32_t getConfInt32(const std::string& key) {
    return Conf::get()->getInt32(key);
}

const uint32_t getConfUInt32(const std::string& key) {
    return Conf::get()->getUInt32(key);
}

const int16_t getConfInt16(const std::string& key) {
    return Conf::get()->getInt16(key);
}

const uint16_t getConfUInt16(const std::string& key) {
    return Conf::get()->getUInt16(key);
}

const int64_t getConfInt64(const std::string& key) {
    return Conf::get()->getInt64(key);
}

const uint64_t getConfUInt64(const std::string& key) {
    return Conf::get()->getUInt64(key);
}

const float getConfFloat(const std::string& key) {
    return Conf::get()->getFloat(key);
}

const double getConfDouble(const std::string& key) {
    return Conf::get()->getDouble(key);
}

const bool getConfBoolean(const std::string& key) {
    return Conf::get()->getBoolean(key);
}

const bool hasCustomizedConfKey(const std::string& key) {
    return Conf::get()->hasCustomizedKey(key);
}

const std::string getAbsoluteFilePath(const std::string& filename) {
    // TODO: path separator should come from detecting operating sytem. We hardcode it for linux/unix temporarily.
    if ((filename.length() > 0) && (filename.at(0) != '/')) {
        if (std::getenv("DERECHO_CONF_FILE")) {
            std::string conf_file = std::getenv("DERECHO_CONF_FILE");
            if (conf_file.find_last_of('/') == std::string::npos) {
                return filename;
            }
            std::string path_prefix = conf_file.substr(0,conf_file.find_last_of('/'));
            return path_prefix + '/' + filename;
        }
    }
    return filename;
}

std::vector<std::string> split_string(const std::string& str, const std::string& delimiter) {
    std::vector<std::string> result;
    std::size_t lastpos = 0;
    std::size_t nextpos = 0;
    while((nextpos = str.find(delimiter, lastpos)) != std::string::npos) {
        result.emplace_back(str.substr(lastpos, nextpos));
        lastpos = nextpos + delimiter.length();
    }
    result.emplace_back(str.substr(lastpos));
    return result;
}

}  // namespace derecho
