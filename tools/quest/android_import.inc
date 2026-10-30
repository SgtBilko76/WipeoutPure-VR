// A release's own setup on the headset (inserted into PortableKit's android_app.cpp by xr_bridge_edits.py, inside
// its PORTABLEKIT_ANDROID_EXTERNAL_DATA block). VR has no document picker, so the player copies files into the
// app's folder on shared storage, /sdcard/Android/data/<app>/files/, with SideQuest or a PC's file manager.

// The disc image the player copied into import/ (any name ending in .iso): checked, moved to game/disc.iso (the
// same storage, so no copy) and the executable prepared from it, as the desktop setup does (install::install). An
// image that is not the supported release stays in import/, with SETUP-ERROR.txt saying why. The test switch
// PURE_IMPORT_GAME_DIR names another folder than game/, so an installed game is left alone.
void import_disc_image(const std::filesystem::path &root) {
    namespace fs = std::filesystem;
    std::error_code ec;
    const fs::path import = root / "import";
    fs::path image;
    for (fs::directory_iterator it(import, ec), end; !ec && it != end; it.increment(ec)) {
        std::string extension = it->path().extension().string();
        std::transform(extension.begin(), extension.end(), extension.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        if (extension == ".iso" && it->is_regular_file(ec)) {
            image = it->path();
            break;
        }
    }
    if (image.empty()) return;
    const char *named = std::getenv("PURE_IMPORT_GAME_DIR");
    const fs::path game = root / (named != nullptr && *named != '\0' ? named : "game");
    const fs::path disc = game / "disc.iso";
    const fs::path error_file = import / "SETUP-ERROR.txt";
    std::printf("[android] setting up from %s\n", image.c_str());
    std::fflush(stdout);
    bool moved = false;
    try {
        portablekit::install::check_image(image);
        fs::create_directories(game);
        fs::remove(disc, ec);
        fs::rename(image, disc);
        moved = true;
        int shown = -1;
        portablekit::install::install(
            disc, portablekit::install::ImageStorage::Copy, game,
            [&shown](const std::string &stage, std::uint64_t done, std::uint64_t total) {
                const int percent = total != 0u ? static_cast<int>(done * 100u / total) : 0;
                if (percent / 25 == shown / 25) return;
                shown = percent;
                std::printf("[android] %s: %d%%\n", stage.c_str(), percent);
            });
        fs::remove(error_file, ec);
        std::printf("[android] the game is set up in %s\n", game.c_str());
    } catch (const std::exception &e) {
        std::printf("[android] setting up from %s failed: %s\n", image.c_str(), e.what());
        if (moved) fs::rename(disc, image, ec);
        if (std::ofstream out(error_file); out)
            out << "Wipeout Pure VR could not use " << image.filename().string() << ":\n" << e.what() << "\n";
    }
    std::fflush(stdout);
}

// The shader pipelines a release ships (assets/pipeline/, taken from a headset that has driven the game): the list
// of every pipeline seen, which the renderer builds in the background at start (the eyes' MSAA ones too), and the
// driver's cache of them, which a driver that does not match simply ignores. Copied once; from then on the files
// grow with the player's own races.
void copy_bundled_pipelines(const std::filesystem::path &root) {
    for (const char *name : {"pipeline_keys.bin", "pipeline_cache.bin"}) {
        const std::filesystem::path target = root / name;
        std::error_code ec;
        if (std::filesystem::exists(target, ec)) continue;
        const std::string asset = std::string("pipeline/") + name;
        std::size_t size = 0;
        void *data = SDL_LoadFile(asset.c_str(), &size);
        if (data == nullptr) continue;
        const std::filesystem::path partial = root / (std::string(name) + ".part");
        {
            std::ofstream out(partial, std::ios::binary | std::ios::trunc);
            out.write(static_cast<const char *>(data), static_cast<std::streamsize>(size));
        }
        SDL_free(data);
        std::filesystem::rename(partial, target, ec);
        std::printf("[android] %s from the APK (%zu bytes)\n", name, size);
    }
}

