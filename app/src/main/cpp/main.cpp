#include <android/log.h>
#include <sys/system_properties.h>
#include <unistd.h>
#include <vector>
#include <string>
#include <fstream>
#include <sstream>
#include <algorithm>

#include "zygisk.hpp"
#include "json.hpp" // Changed to just json.hpp
#include "dobby.h"

#define LOGD(...) __android_log_print(ANDROID_LOG_DEBUG, "PIF/Native", __VA_ARGS__)

#define JSON_FILE_PATH "/data/adb/modules/playintegrityfix/pif.json"
#define CUSTOM_JSON_FILE_PATH "/data/adb/modules/playintegrityfix/custom.pif.json"
#define TARGET_LIST_PATH "/data/adb/modules/playintegrityfix/target.txt"

static int verboseLogs = 0;
static int spoofBuild = 1;
static int spoofProps = 1;
static int spoofProvider = 1;
static int spoofSignature = 0;

static std::map<std::string, std::string> jsonProps;

typedef void (*T_Callback)(void *, const char *, const char *, uint32_t);

static std::map<void *, T_Callback> callbacks;

static std::vector<std::string> targetApps;

static void readTargetList() {
    std::ifstream targetFile(TARGET_LIST_PATH);
    if (targetFile.is_open()) {
        std::string line;
        while (std::getline(targetFile, line)) {
            targetApps.push_back(line);
        }
        targetFile.close();
    } else {
        LOGD("Failed to open target list file: %s", TARGET_LIST_PATH);
    }
}

static void modify_callback(void *cookie, const char *name, const char *value, uint32t serial) {
    if (cookie == nullptr || name == nullptr || value == nullptr || !callbacks.contains(cookie)) return;

    const char *oldValue = value;

    std::string prop(name);

    if (jsonProps.count(prop)) {
        value = jsonProps[prop].c_str();
    } else {
        for (const auto &p: jsonProps) {
            if (p.first.starts_with("*") && prop.starts_with(p.first.substr(1))) {
                value = p.second.c_str();
                break;
            }
        }
    }

    if (verboseLogs) {
        LOGD("Prop %s: %s -> %s", name, oldValue, value);
    }

    callbacks[cookie](cookie, name, value, serial);
};

class PlayIntegrityFix : public zygisk::ModuleBase {
public:
    void onLoad(zygisk::Api *api, JNIEnv *env) override {
        this->api = api;
        this->env = env;
        readTargetList();
    }

    void postAppSpecialize(const zygisk::AppSpecializeArgs *args) override {
        auto rawProcess = env->GetStringUTFChars(args->nice_name, nullptr);
        std::string pkgName = rawProcess;
        env->ReleaseStringUTFChars(args->nice_name, rawProcess);

        std::string jsonPath;
        if (std::find(targetApps.begin(), targetApps.end(), pkgName) != targetApps.end()) {
            jsonPath = CUSTOM_JSON_FILE_PATH;
        } else {
            jsonPath = JSON_FILE_PATH;
        }

        std::ifstream jsonFile(jsonPath);
        std::stringstream buffer;
        buffer << jsonFile.rdbuf();
        std::string jsonContent = buffer.str();

        std::vector<char> jsonVector(jsonContent.begin(), jsonContent.end());

        std::string jsonString(jsonVector.cbegin(), jsonVector.cend());
        nlohmann::json json = nlohmann::json::parse(jsonString, nullptr, false, true);

        if (json.is_discarded()) {
            LOGD("JSON is discarded, not continuing");
            return;
        }

        if (json.contains("verboseLogs") && json["verboseLogs"].is_number_integer())
            verboseLogs = json["verboseLogs"].get<int>();
        if (json.contains("spoofBuild") && json["spoofBuild"].is_number_integer())
            spoofBuild = json["spoofBuild"].get<int>();
        if (json.contains("spoofProps") && json["spoofProps"].is_number_integer())
            spoofProps = json["spoofProps"].get<int>();
        if (json.contains("spoofProvider") && json["spoofProvider"].is_number_integer())
            spoofProvider = json["spoofProvider"].get<int>();
        if (json.contains("spoofSignature") && json["spoofSignature"].is_number_integer())
            spoofSignature = json["spoofSignature"].get<int>();

        if (json.contains("props") && json["props"].is_object()) {
            for (auto it = json["props"].begin(); it != json["props"].end(); ++it) {
                jsonProps[it.key()] = it.value().get<std::string>();
            }
        }

        if (spoofProps) {
            void *cookie = nullptr;
            T_Callback original_callback = nullptr;

            DobbyHook((void *) __system_property_read_callback, (void *) modify_callback, (void **) &original_callback);

            if (original_callback != nullptr) {
                callbacks[cookie] = original_callback;
            }
        }
    }

private:
    zygisk::Api *api = nullptr;
    JNIEnv *env = nullptr;
};

static void companion(int fd) {
    long jsonSize = 0;
    std::vector<char> jsonVector;

    FILE *json = fopen(CUSTOM_JSON_FILE_PATH, "r");
    if (!json)
        json = fopen(JSON_FILE_PATH, "r");

    if (json) {
        fseek(json, 0, SEEK_END);
        jsonSize = ftell(json);
        fseek(json, 0, SEEK_SET);

        jsonVector.resize(jsonSize);
        fread(jsonVector.data(), 1, jsonSize, json);

        fclose(json);
    }

    write(fd, &jsonSize, sizeof(long));
    write(fd, jsonVector.data(), jsonSize);

    jsonVector.clear();
}

REGISTER_ZYGISK_MODULE(PlayIntegrityFix)

REGISTER_ZYGISK_COMPANION(companion)
