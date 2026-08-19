#pragma once

// C++ standard
#include <filesystem>
#include <fstream>
#include <map>
#include <optional>
#include <stdexcept>
#include <string>
#include <variant>
#include <vector>

// Boost
#include <boost/json.hpp>

namespace Euclid::Core {

    // Forward declare for recursive variant
    struct ConfigObject;

    // Recursive variant — supports nested objects
    using ConfigValue = std::variant<
        bool,
        long,
        double,
        std::string,
        std::shared_ptr<ConfigObject> // ← nested object
    >;

    struct ConfigObject {
        std::map<std::string, ConfigValue> values;

        // Convenience accessors
        [[nodiscard]]
        bool has(const std::string &key) const {
            return values.contains(key);
        }

        template<typename T>
        T get(const std::string &key) const {
            return std::get<T>(values.at(key));
        }

        template<typename T>
        T getOr(const std::string &key, T def) const noexcept {
            try {
                auto it = values.find(key);
                if (it == values.end()) return def;
                return std::get<T>(it->second);
            } catch (...) { return def; }
        }
    };

    /**
     * @brief Retrieves all child objects under a specified path as named maps.
     *
     * This method processes the configuration structure to locate all direct children of the
     * specified path and structures them as named key-value maps, where each key is the name of
     * the child and the value is the corresponding configuration object.
     * Throws an exception if the path does not resolve to a valid object node.
     *
     * @param path The dot-separated path to the object node containing child objects.
     * @return A map where the keys represent the names of child objects, and values represent
     *         their corresponding configurations as nested key-value maps.
     * @throws std::runtime_error If the specified path does not resolve to an object node.
     */
    class Configuration {

    public:
        /**
         * @brief Provides a singleton instance of the Configuration class
         *
         * Ensures a single instance of the Configuration class is created and globally accessible.
         *
         * @return Reference to the singleton Configuration instance
         */
        static Configuration &instance() {
            static Configuration inst;
            return inst;
        }

        /**
         * @brief Manages application configuration settings
         *
         * Contains methods and properties to handle configurable parameters
         * and settings of the application.
         *
         * @return Reference to the Configuration instance
         */
        Configuration(const Configuration &) = delete;

        /**
         * @brief Overloaded operator for performing a specific operation
         *
         * Implements a custom behavior when the operator is used with a particular class.
         *
         * @param configuration The left-hand side operand involved in the operation
         * @return Result of the operation as determined by the implementation
         */
        Configuration &operator=(const Configuration &configuration) = delete;

        /**
         * @brief Loads the configuration from a specified file path.
         *
         * Reads the configuration data from the given file path, parses it as JSON with
         * support for comments and trailing commas, and stores the information internally.
         * Throws an exception if the file does not exist, cannot be opened, or if there are
         * parsing errors.
         *
         * @param path The filesystem path to the configuration file to be loaded.
         * @throw std::runtime_error If the file does not exist, cannot be opened, or if JSON
         *        parsing fails.
         */
        void load(const std::filesystem::path &path);

        /**
         * @brief Reloads the configuration from the previously loaded file path.
         *
         * This method re-reads the configuration file and updates internal state.
         * Throws an exception if no configuration file path has been set prior to calling this method.
         *
         * @throws std::runtime_error if the configuration file path is empty.
         */
        void reload();

        /**
         * @brief Saves the current configuration to the previously loaded file.
         *
         * This method writes the configuration data to the file specified during
         * the initial load or a previous save operation. If no file path is
         * available, an exception is thrown to indicate that a file must be
         * specified using the `saveTo(path)` method instead.
         *
         * @throws std::runtime_error If no configuration file path has been set.
         */
        void save() const;

        /**
         * @brief Save the configuration to a file at the specified path.
         *
         * This method writes the configuration data to the given file path.
         * If the parent directories of the specified path do not exist, they are created automatically.
         * The configuration data is written in a pretty-printed, indented format.
         *
         * @param path  Filesystem path where the configuration should be saved
         * @throws std::runtime_error if the file cannot be opened for writing or if the write operation fails
         */
        void saveTo(const std::filesystem::path &path) const;

        /**
         * @brief Retrieves a value or resource.
         *
         * This method is used to fetch and return the requested value or resource.
         * The specifics of what is retrieved depend on the implementation.
         *
         * @return The requested value or resource
         */
        template<typename T>
        T get(const std::string &path) const {
            const auto *node = resolvePath(path);
            if (!node) throw std::runtime_error("Config key not found: " + path);
            return extractValue<T>(*node, path);
        }

        /**
         * @brief Retrieves the value at the specified configuration path or returns a default value.
         *
         * This method attempts to resolve the given path in the configuration structure. If the path is not found
         * or an error occurs during resolution, the provided default value is returned. The method is safe to use
         * as it does not throw exceptions.
         *
         * @tparam T The type of the value to retrieve.
         * @param path The configuration path to resolve.
         * @param def The default value to return if the path cannot be resolved or an error occurs.
         * @return The value at the specified path if it exists and is accessible, otherwise the default value.
         */
        template<typename T>
        T getOr(const std::string &path, T def) const noexcept {
            try {
                const auto *node = resolvePath(path);
                if (!node) return def;
                return extractValue<T>(*node, path);
            } catch (...) {
                return def;
            }
        }

        /**
         * @brief Sets a value for the specified key in the configuration
         *
         * Updates the configuration by assigning the provided value to the given key.
         *
         * @param path The unique identifier for the configuration setting
         * @param value The value to associate with the given key
         */
        template<typename T>
        void set(const std::string &path, T value);

        /**
         * @brief Checks if a certain condition is met.
         *
         * The `has` method evaluates a certain criterion based on the provided input
         * and determines whether the corresponding condition is satisfied.
         *
         * @param path The key or identifier to check against the condition.
         * @return True if the condition is met for the given key, otherwise false.
         */
        [[nodiscard]]
        bool has(const std::string &path) const noexcept {
            return resolvePath(path) != nullptr;
        }

        /**
         * @brief Get all child key names under a dot-separated path
         *
         * @par Example:
         * @code
         * // JSON: { "awsmock": { "modules": { "s3": {...}, "sqs": {...} } } }
         * auto keys = cfg.getKeys("awsmock.modules");
         * // → ["s3", "sqs"]
         * @endcode
         *
         * @param path  dot-separated path to an object node
         * @return vector of child key names, empty if path missing or not object
         */
        [[nodiscard]]
        std::vector<std::string> getKeys(const std::string &path) const noexcept;

        /**
         * @brief Get a single object node as a flat key→ConfigValue map
         *
         * @par Example:
         * @code
         * auto props = cfg.getObject("awsmock.modules.s3");
         * bool active = std::get<bool>(props["active"]);
         * auto socket = std::get<std::string>(props["socket"]);
         * @endcode
         *
         * @param path  dot-separated path to an object node
         * @throws std::runtime_error if a path isn't found or not an object
         */
        [[nodiscard]]
        std::map<std::string, ConfigValue> getObject(const std::string &path) const;

        /**
         * @brief Get all child objects under a path as named maps
         *
         * @par Example JSON:
         * @code
         * {
         *   "awsmock": {
         *     "modules": {
         *       "s3":  { "active": true,  "socket": "/var/run/awsmock/s3.sock",  "port": 4572 },
         *       "sqs": { "active": true,  "socket": "/var/run/awsmock/sqs.sock", "port": 4567 },
         *       "sns": { "active": false, "socket": "/var/run/awsmock/sns.sock", "port": 4568 }
         *     }
         *   }
         * }
         * @endcode
         *
         * @par Usage:
         * @code
         * for (auto& [name, props] : cfg.getObjects("awsmock.modules")) {
         *     bool active = std::get<bool>(props.at("active"));
         *     auto socket = std::get<std::string>(props.at("socket"));
         *     int  port   = static_cast<int>(std::get<long>(props.at("port")));
         * }
         * @endcode
         *
         * @param path  dot-separated path to a parent object node
         * @throws std::runtime_error if path not found or not an object
         */
        [[nodiscard]]
        std::vector<std::pair<std::string, std::map<std::string, ConfigValue> > >
        getObjects(const std::string &path) const;

        /**
         * @brief Get a JSON array node as a vector of typed values
         *
         * @par Example:
         * @code
         * // JSON: { "awsmock": { "tags": ["production", "eu-west-1"] } }
         * auto tags = cfg.getArray<std::string>("awsmock.tags");
         * @endcode
         *
         * @tparam T  int | long | double | bool | std::string
         * @throws std::runtime_error if path not found, not array, or type mismatch
         */
        template<typename T>
        [[nodiscard]]
        std::vector<T> getArray(const std::string &path) const {
            const auto *node = resolvePath(path);
            if (!node) throw std::runtime_error("Config key not found: " + path);
            if (!node->is_array()) throw std::runtime_error("Config key '" + path + "' is not an array");

            std::vector<T> result;
            result.reserve(node->get_array().size());
            for (const auto &elem: node->get_array()) result.push_back(extractValue<T>(elem, path + "[]"));
            return result;
        }

        /**
         * @brief Serializes the current configuration state to a JSON-formatted string.
         *
         * This method converts the internal configuration data into a string
         * representation using the JSON format. The serialized output reflects the
         * current state of the configuration, making it suitable for debugging,
         * logging, or exporting purposes.
         *
         * @return A JSON-formatted string representing the serialized configuration.
         */
        [[nodiscard]]
        std::string dump() const;

        /**
         * @brief Retrieves the file path associated with the configuration.
         *
         * This method returns the file path currently used by the configuration
         * manager. The file path typically points to the location of the
         * configuration file on the filesystem.
         *
         * @return The file path as a `std::filesystem::path` object.
         */
        [[nodiscard]]
        std::filesystem::path filePath() const { return _filePath; }

    private:
        /**
         *
         */
        Configuration() = default;

        /**
         * @brief Resolves a JSON value by navigating a hierarchical path in the configuration data.
         *
         * This method traverses the configuration tree based on a dotted path string, returning
         * a pointer to the JSON value at the specified location. If the path is invalid or points
         * to a non-existent key, the method returns nullptr.
         *
         * @param path The dotted path string used to locate a value in the hierarchical configuration.
         *             Each segment is separated by a dot (e.g., "foo.bar.baz").
         * @return A pointer to the `boost::json::value` found at the specified path, or nullptr
         *         if the path cannot be resolved.
         */
        [[nodiscard]]
        const boost::json::value *resolvePath(const std::string &path) const noexcept;

        /**
         * @brief Resolves or creates a nested JSON object path within the configuration.
         *
         * This method traverses the JSON structure starting from the root, using the
         * provided path. Each segment of the path represents a key in the JSON object.
         * If a key does not exist, it will create a nested object for that segment.
         * The resulting JSON value (at the end of the path) is returned as a pointer.
         *
         * @param path The dot-separated path representing the hierarchy to resolve or create.
         * @return A pointer to the nested JSON value at the specified path.
         */
        boost::json::value *resolveOrCreatePath(const std::string &path);

        /**
         * @brief Extracts a value of type T from a JSON object using a specified path.
         *
         * This method traverses a JSON object based on the provided path and attempts
         * to extract the value at the specified location. The path uses a dot-separated
         * notation to denote hierarchy within the JSON structure.
         *
         * @tparam T The type to which the extracted value will be converted.
         * @param v The JSON object from which the value will be extracted.
         * @param path The dot-separated path specifying the location of the value to be extracted.
         * @return The value extracted from the JSON object, converted to the specified type T.
         */
        template<typename T>
        static T extractValue(const boost::json::value &v, const std::string &path);

        /**
         * @brief Formats a JSON value into a human-readable, pretty-printed string.
         *
         * This method converts the supplied JSON value into a string representation
         * with proper indentation for better readability. The formatting is applied
         * recursively to all nested JSON objects and arrays.
         *
         * @param root The root JSON value to be pretty-printed.
         * @return A pretty-printed string representation of the given JSON value.
         */
        static std::string prettyPrint(const boost::json::value &root);

        /**
         * @brief Outputs a formatted, human-readable representation of a JSON value to a stream.
         *
         * This method recursively processes a JSON value, formatting it with proper indentation
         * to improve readability. It handles objects, arrays, and scalar values, adjusting
         * the indentation for nested structures.
         *
         * @param v The JSON value to be pretty-printed.
         * @param out The output stream where the formatted JSON will be written.
         * @param indent The current indentation level (used internally for recursive calls).
         */
        static void prettyPrintValue(const boost::json::value &v, std::ostream &out, int indent);

        /**
         * @brief Internal storage for the hierarchical configuration data.
         *
         * The `_root` variable holds the primary JSON structure representing all configuration
         * details of the application. It serves as the core data container for parsing,
         * accessing, and manipulating configuration settings. This variable is typically
         * utilized as the central point for managing serialized configuration input and output.
         */
        boost::json::value _root{};

        /**
         * @brief Stores the file system path used for configuration purposes.
         *
         * The `_filePath` variable holds the path to a file utilized by the application
         * for storing or retrieving configuration settings. It uses the `std::filesystem::path`
         * type to ensure compatibility and ease of interaction with filesystem operations.
         */
        std::filesystem::path _filePath;
    };

    // ── extractValue specializations (inline in header) ──────────────────────

    template<>
    inline int Configuration::extractValue<int>(
        const boost::json::value &v, const std::string &path) {
        if (v.is_int64()) return static_cast<int>(v.get_int64());
        if (v.is_uint64()) return static_cast<int>(v.get_uint64());
        if (v.is_double()) return static_cast<int>(v.get_double());
        throw std::runtime_error("Config key '" + path + "' is not an integer");
    }

    template<>
    inline long Configuration::extractValue<long>(
        const boost::json::value &v, const std::string &path) {
        if (v.is_int64()) return v.get_int64();
        if (v.is_uint64()) return static_cast<long>(v.get_uint64());
        if (v.is_double()) return static_cast<long>(v.get_double());
        throw std::runtime_error("Config key '" + path + "' is not a long");
    }

    template<>
    inline double Configuration::extractValue<double>(
        const boost::json::value &v, const std::string &path) {
        if (v.is_double()) return v.get_double();
        if (v.is_int64()) return static_cast<double>(v.get_int64());
        if (v.is_uint64()) return static_cast<double>(v.get_uint64());
        throw std::runtime_error("Config key '" + path + "' is not a double");
    }

    template<>
    inline bool Configuration::extractValue<bool>(
        const boost::json::value &v, const std::string &path) {
        if (v.is_bool()) return v.get_bool();
        throw std::runtime_error("Config key '" + path + "' is not a bool");
    }

    template<>
    inline std::string Configuration::extractValue<std::string>(
        const boost::json::value &v, const std::string &path) {
        if (v.is_string()) return std::string(v.get_string());
        if (v.is_int64()) return std::to_string(v.get_int64());
        if (v.is_uint64()) return std::to_string(v.get_uint64());
        if (v.is_double()) return std::to_string(v.get_double());
        if (v.is_bool()) return v.get_bool() ? "true" : "false";
        throw std::runtime_error("Config key '" + path + "' is not a string");
    }

    // ── set specializations (inline in header) ────────────────────────────────

    template<>
    inline void Configuration::set<int>(const std::string &p, int v) {
        *resolveOrCreatePath(p) = boost::json::value(static_cast<std::int64_t>(v));
    }

    template<>
    inline void Configuration::set<long>(const std::string &p, long v) {
        *resolveOrCreatePath(p) = boost::json::value(static_cast<std::int64_t>(v));
    }

    template<>
    inline void Configuration::set<double>(const std::string &p, double v) {
        *resolveOrCreatePath(p) = boost::json::value(v);
    }

    template<>
    inline void Configuration::set<bool>(const std::string &p, bool v) {
        *resolveOrCreatePath(p) = boost::json::value(v);
    }

    template<>
    inline void Configuration::set<std::string>(const std::string &p, std::string v) {
        *resolveOrCreatePath(p) = boost::json::value(
            boost::json::string_view(v));
    }

} // namespace Euclid::Core