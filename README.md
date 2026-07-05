# llama.cpp Tray

A small Linux system tray app for monitoring and managing local
`llama.cpp` / `llama-swap` model servers.

The app is built with C++ and Qt 6. It shows loaded and available models,
can trigger model load/unload actions, and stores connection settings with
`QSettings`.

<img width="199" height="266" alt="image" src="https://github.com/user-attachments/assets/0e1bf057-8ca5-45e7-b0ba-9fbcd14b4c7d" />

## Features

- System tray menu for loaded and available models
- HTTP integration with `llama-swap` and `llama-server`
- Background refresh and command execution
- Configurable server URL, config path, and launch arguments
- JSON-based UI translations, with English as the default
- Bundled tray icons and desktop entry

## Requirements

- Linux with a system tray
- CMake 3.21 or newer
- Qt 6.5 or newer with Core, Gui, Widgets, Network, and Concurrent modules
- A running `llama.cpp` compatible server for model management

## Build

```bash
cmake -S . -B build
cmake --build build -j
```

## Run

```bash
./build/llama_cpp_tray
```

Optional arguments:

```bash
./build/llama_cpp_tray --url http://127.0.0.1:8082 --refresh-seconds 5 --language en
```

## Translations

UI translations live in `translations/`. To add a language, add one
`<language>.json` file with the same keys as `translations/en.json`, then run
the app with `--language <language>`.

## Project Layout

- `src/` - application source code
- `assets/` - tray icons
- `resources/` - Qt resource file
- `translations/` - UI translation files
- `llama-cpp-tray.desktop` - Linux desktop entry

## License

MIT
