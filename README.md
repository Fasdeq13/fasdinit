# fasdinit — Ultra-Fast Asynchronous Rust Init System

[English](#english) | [Русский](#русский)

---

## English

**fasdinit** is a modern, modular, and lightning-fast system initialization engine and service supervisor built from scratch in safe, high-performance **Rust**. Designed as a predictable, memory-safe alternative to `systemd`, it brings high-concurrency architecture to early Linux userspace with zero daemon overhead and absolute reliability.

By combining the lightweight, transparent configuration architecture inspired by **FreeBSD rc.d** with modern Linux kernel primitives and a safe `libc::poll` event loop, **fasdinit** eliminates early-boot shell performance bottlenecks, achieving near-instantaneous desktop telemetry and service execution.

### ⚡ Key Architectural Features

* **Kahn's DAG Scheduler:** Resolves complex `# REQUIRE` and `# PROVIDE` service dependencies at microsecond scale using a compiled Directed Acyclic Graph (DAG) solver.
* **Zero-CPU Self-Pipe Event Loop:** Utilizes a highly optimized `libc::poll` loop over a secure self-pipe architecture, executing incoming `fasdctl` IPC requests and catching system signals instantly with 0% idle CPU overhead.
* **Native Plain-Text Log Interception:** The process supervisor automatically captures child service standard descriptors (`stdout`/`stderr`) and routes them directly into individual plain-text logfiles at `/var/log/fasdinit/`.
* **Zero-Dependency Core:** Built using only essential, low-level Rust primitives and lightweight serialization, ensuring a tiny binary footprint ideal for root filesystems.
* **Safe Privilege Dropping:** Low-level `fork`/`execvp` routines handle secure process spawning, descriptor redirection, and execution context isolation.

### 📂 Repository Structure

* **`Cargo.toml`** — Monolithic workspace configuration optimized for size (`panic = "abort"`, Link-Time Optimization, automatic stripping).
* **`src/main.rs`** — Root PID 1 engine handling early mount (`/proc`, `/sys`, `/dev`), asynchronous signal dispatching, and master control loops.
* **`src/ctl.rs`** — High-efficiency CLI supervisor controller (`fasdctl`) communicating via a lightweight UNIX socket (`/run/fasdinit.sock`).
* **`src/config.rs`** — Flat text parser managing `/etc/fasdinit/rc.conf` and extracting `# PROVIDE` / `# REQUIRE` metadata from service units.
* **`src/manager.rs`** — The brain of the init system, managing service runtime states and computing deterministic boot orders.
* **`src/executor.rs`** — Low-level POSIX process launcher, child process reaper (`waitpid`), and system state synchronizer.

---

## Русский

**fasdinit** — это современная, модульная и молниеносная система инициализации и супервизор сервисов, написанные с нуля на безопасном и высокопроизводительном **Rust**. Спроектированная как предсказуемая и безопасная по памяти альтернатива `systemd`, она привносит архитектуру высокой конкурентности на самый ранний этап загрузки Linux, гарантируя нулевой оверхед и абсолютную надежность PID 1.

Сочетая легковесную и прозрачную структуру конфигурации в стиле **FreeBSD rc.d** с низкоуровневыми примитивами ядра Linux и событийно-ориентированным циклом `libc::poll`, **fasdinit** полностью устраняет задержки интерпретаторов командной строки при старте ОС, обеспечивая практически мгновенный запуск системных компонентов.

### ⚡ Ключевые особенности архитектуры

* **Планировщик на алгоритме Кана:** Разрешает сложные цепочки зависимостей `# REQUIRE` и `# PROVIDE` на микросекундном уровне с помощью детерминированного обхода направленного ациклического графа (DAG).
* **Энергоэффективный Self-Pipe цикл:** Поток PID 1 «замораживается» в ядре через `libc::poll` и просыпается за 0 миллисекунд только при поступлении команд от `fasdctl` или системных сигналов, потребляя **ровно 0% процессора** в режиме ожидания.
* **Перехват логов «из коробки»:** Супервизор процессов автоматически перенаправляет стандартные дескрипторы (`stdout`/`stderr`) запущенных служб в отдельные текстовые лог-файлы по пути `/var/log/fasdinit/`.
* **Zero-Dependency ядро:** Написана с использованием только базовых системных вызовов и легковесной сериализации, что гарантирует минимальный размер бинарного файла, критически важный для корневой ФС.
* **Безопасный сброс привилегий:** Низкоуровневые процедуры `fork`/`execvp` изолируют контекст выполнения служб и поддерживают запуск демонов от неполноправных пользователей системы.

### 📂 Структура репозитория

* **`Cargo.toml`** — Конфигурация сборки, оптимизированная под минимальный размер бинарников (`panic = "abort"`, LTO, автоматический strip символов).
* **`src/main.rs`** — Ядро PID 1, отвечающее за раннее монтирование (`/proc`, `/sys`, `/dev`), безопасную обработку сигналов и главный цикл.
* **`src/ctl.rs`** — Высокоэффективная утилита управления (`fasdctl`), взаимодействующая с основным демоном через UNIX-сокет `/run/fasdinit.sock`.
* **`src/config.rs`** — Парсер плоских конфигурационных файлов `/etc/fasdinit/rc.conf` и заголовков скриптов служб.
* **`src/manager.rs`** — Диспетчер состояний, управляющий жизненным циклом служб и рассчитывающий порядок их загрузки.
* **`src/executor.rs`** — Низкоуровневый исполнитель процессов, зачищающий систему от зомби-процессов (`waitpid`) и синхронизирующий диски.
