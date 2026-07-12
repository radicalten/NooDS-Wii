#include <sys/stat.h>
#include "core.h"

int Settings::directBoot  = 1;
int Settings::romInRam    = 0;
int Settings::fpsLimiter  = 0;
int Settings::frameskip   = 0;
int Settings::threaded2D  = 1;
int Settings::threaded3D  = 1;
int Settings::highRes3D   = 0;
int Settings::screenGhost = 0;
int Settings::emulateAudio = 1;
int Settings::audio16Bit  = 1;
int Settings::monoAudio   = 0;   // default: stereo
int Settings::savesFolder  = 0;
int Settings::statesFolder = 1;
int Settings::cheatsFolder = 1;
int Settings::screenFilter = 2;
int Settings::arm7Hle     = 0;
int Settings::dsiMode     = 0;

std::string Settings::bios9Path    = "bios9.bin";
std::string Settings::bios7Path    = "bios7.bin";
std::string Settings::firmwarePath = "firmware.bin";
std::string Settings::gbaBiosPath  = "gba_bios.bin";
std::string Settings::sdImagePath  = "sd.img";
std::string Settings::basePath     = ".";

std::vector<Setting> Settings::settings = {
    Setting("directBoot",   &directBoot,   false),
    Setting("romInRam",     &romInRam,     false),
    Setting("fpsLimiter",   &fpsLimiter,   false),
    Setting("frameskip",    &frameskip,    false),
    Setting("threaded2D",   &threaded2D,   false),
    Setting("threaded3D",   &threaded3D,   false),
    Setting("highRes3D",    &highRes3D,    false),
    Setting("screenGhost",  &screenGhost,  false),
    Setting("emulateAudio", &emulateAudio, false),
    Setting("audio16Bit",   &audio16Bit,   false),
    Setting("monoAudio",    &monoAudio,    false),
    Setting("savesFolder",  &savesFolder,  false),
    Setting("statesFolder", &statesFolder, false),
    Setting("cheatsFolder", &cheatsFolder, false),
    Setting("screenFilter", &screenFilter, false),
    Setting("arm7Hle",      &arm7Hle,      false),
    Setting("dsiMode",      &dsiMode,      false),
    Setting("bios9Path",    &bios9Path,    true),
    Setting("bios7Path",    &bios7Path,    true),
    Setting("firmwarePath", &firmwarePath, true),
    Setting("gbaBiosPath",  &gbaBiosPath,  true),
    Setting("sdImagePath",  &sdImagePath,  true),
};

void Settings::add(std::vector<Setting> &settings) {
    Settings::settings.insert(
        Settings::settings.end(), settings.begin(), settings.end());
}

bool Settings::load(std::string path) {
    // Set the base path and ensure all folders exist.
    mkdir((basePath = path).c_str() MKDIR_ARGS);
    mkdir((basePath + "/saves").c_str()  MKDIR_ARGS);
    mkdir((basePath + "/states").c_str() MKDIR_ARGS);
    mkdir((basePath + "/cheats").c_str() MKDIR_ARGS);

    // Open the settings file; write defaults if it does not exist.
    FILE *file = fopen((basePath + "/noods.ini").c_str(), "r");
    if (!file) {
        Settings::bios9Path    = basePath + "/bios9.bin";
        Settings::bios7Path    = basePath + "/bios7.bin";
        Settings::firmwarePath = basePath + "/firmware.bin";
        Settings::gbaBiosPath  = basePath + "/gba_bios.bin";
        Settings::sdImagePath  = basePath + "/sd.img";
        Settings::save();
        return false;
    }

    // Read each key=value line.
    char data[512];
    while (fgets(data, 512, file) != nullptr) {
        std::string line  = data;
        size_t      split = line.find('=');
        if (split == std::string::npos) continue;

        std::string name = line.substr(0, split);
        for (size_t i = 0; i < settings.size(); i++) {
            if (name != settings[i].name) continue;
            // Strip the trailing newline from the value.
            std::string value = line.substr(split + 1,
                                            line.size() - split - 2);
            if (settings[i].isString)
                *(std::string*)settings[i].value = value;
            else if (!value.empty() &&
                     value[0] >= '0' && value[0] <= '9')
                *(int*)settings[i].value = stoi(value);
            break;
        }
    }

    fclose(file);
    return true;
}

bool Settings::save() {
    FILE *file = fopen((basePath + "/noods.ini").c_str(), "w");
    if (!file) return false;

    for (size_t i = 0; i < settings.size(); i++) {
        std::string value = settings[i].isString
            ? *(std::string*)settings[i].value
            : std::to_string(*(int*)settings[i].value);
        fprintf(file, "%s=%s\n", settings[i].name.c_str(), value.c_str());
    }

    fclose(file);
    return true;
}
