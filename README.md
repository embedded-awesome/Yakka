# Yakka — the embedded builder

[![Build Status](https://img.shields.io/badge/build-passing-brightgreen)]() [![Release](https://img.shields.io/badge/release-latest-blue)]() [![License](https://img.shields.io/badge/license-Apache--2.0-blue)]()

**Yakka is a data‑first build system for embedded projects that models code, tools, and artifacts as reusable components and blueprints so you can describe all parts of a system with YAML, and template powered transforms.**

---

### Table of Contents
- Quick summary
- Why Yakka
- Features
- Quickstart
- Examples and docs
- Usage and CLI
- Blueprints and component schema
- Contributing
- License and maintainers

---

### Quick summary
Yakka makes embedded builds **declarative, composable, and auditable**. Instead of imperative scripts, you describe components (sources, toolchains, libraries) and how they relate to each other, and define blueprints (templates or small transforms) that produce data or artifacts. Yakka resolves components, evalutates blueprints, and runs the minimal steps required to produce any kind of output needed to design, develop, release, and maintain embedded software.

---

### Why Yakka
- **Readable**: build logic lives in YAML and templates, not opaque shell scripts.  
- **Composable**: treat toolchains, libraries, and apps as first‑class components that can be reused across projects.  
- **Auditable**: the entire build graph is data; it’s easy to inspect, diff, and version.  
- **Toolchain agnostic**: integrate GCC, Clang, or custom toolchains via blueprints.
- **Extensible**: add domain or tool extensions (Doxygen, Renode, Qemu, Devicetree, config) by defining simple blueprints.
- **Browser accessible**: built-in HTTP server exposes data to web components for custom GUI.

---

### Features
- **Data‑first components**: everything is a component with metadata and blueprints.  
- **Blueprints**: templates or build steps included in components that implement transforms (compile, link, generate_devicetree).  
- **Human‑friendly formats**: YAML/JSON for data, Jinja for templates.
- **Built-in commands**: Cross platform support for common blueprint actions (file system access, regex, hashing, encryption, binary manipulation)

---

### Quickstart

**Install**  
No installation step is needed. Download and store Yakka in your development repository so developers can simply clone and start being productive immediately.

```bash
# Example: download a release binary
curl -L -o yakka https://github.com/embedded-awesome/Yakka/releases/latest/download/yakka-[linux|macos|windows]
chmod +x yakka-[linux|macos|windows]
./yakka-[linux|macos|windows]
```

**Minimal project layout**
```
workspace/
├─ my_app.yakka
├─ application.h
├─ main.c
└─ yakka
```

**Example component: components/my_app.yakka**
```yaml
name: My project
version: 0.1.0

sources:
  - main.c

includes:
  global:
    - .
```

**Run a link build**
```bash
# Resolve components and run the link blueprint from the toolchain
./yakka link! my_app gcc_arm
```

**Expected result**
- Yakka resolves `my_app` and `gcc_arm`, runs the `link` blueprint, and produces `output/my_app-gcc_arm/project.elf` 
- View `output/my_app-gcc_arm/project_summary.json` for all component information
- View `yakka.log` for detailed log of last action 

---

### Examples and docs
- **examples/** — include runnable projects demonstrating:
  - Bare‑metal C app with GCC toolchain.  
  - Multi‑component app (app + HAL + BSP).  
  - Custom blueprint showing packaging or OTA artifact creation.  
- **docs/** — expand on component schema, blueprint syntax, and best practices.  
- Add a `README` inside each example explaining how to run it and what to expect.

---

### Usage and CLI
Common commands and patterns (adjust to your CLI implementation):
- `yakka list` — list discovered components and their types.  
- `yakka` — show command usage and flags.

---

### Blueprints and component schema
- **Blueprints** are the small, focused transforms attached to components. They can be:
  - Jinja templates that render a command list.  
  - Inline scripts executed in a sandboxed environment.  
  - References to external scripts or tools.
- **Component schema** (recommended keys):
  - `name` - descriptive identity (the only mandatory key)
  - `requires`, `provides`, `supports` — relationships to other components and features
  - `blueprints` — map of blueprint names to templates or scripts.

Keep blueprints small and composable; prefer many focused blueprints over one large monolith.

---

### Contributing
- **CONTRIBUTING.md** — include contribution guidelines, testing, and commit message conventions.  
- **CODE_OF_CONDUCT.md** — set expectations for community behavior.  
- Add tests under `test/` and examples under `examples/`.  
- Use feature branches and open pull requests with a clear description and example usage.

---

### License and maintainers
**License:** Apache‑2.0
**Maintainers:**
 - nik@embedded-awesome.org

---