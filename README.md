# fasdinit — Ultra-Fast Asynchronous C++20 Init System
### [English](#english) | [Русский](#русский)

---

## English

**fasdinit** is a modern, modular, and lightning-fast system initialization engine and service supervisor built from scratch in C++20. Designed as a high-performance, predictable alternative to `systemd` and `dinit`, it brings high-concurrency architecture to early Linux userspace with zero daemon overhead and absolute reliability.

By combining the lightweight, scriptless folder architecture inspired by `runit` with modern Linux kernel primitives (`signalfd`, non-blocking `select` loops, cgroups v2), **fasdinit** eliminates early-boot shell performance bottlenecks, achieving near-instantaneous desktop telemetry.

### ⚡ Key Architectural Features
* **True Non-Blocking Parallel Scheduler:** Independent hardware and service layers shoot into CPU cores concurrently, bounded only by the read speed of your storage device.
* **Microsecond-Scale Byte-Packed State IPC (`fasdctl`):** Toggling services sends a single raw byte straight into non-blocking, kernel-level FIFO pipes managed via lightweight POSIX loops.
* **Native Plain-Text Log Interception:** The process supervisor automatically captures child service standard descriptors and routes them directly into individual plain-text logfiles at `/var/log/fasdinit/`.
* **Smart Display Manager & TTY Cascades:** Automatically probes system paths for modern targets (`sddm`, `lightdm`, `gdm`, `lxdm`). If no compositor is found, it silently falls back to interactive terminal login shells.
* **Out-of-the-Box cgroups v2 Trees:** The root PID 1 engine automatically instantiates a unified Linux kernel cgroups v2 tree layout at boot, wrapping sub-processes within a prioritized `system.slice`.

### 📂 Repository Structure
* `src/fasdinit.cpp` — Root PID 1 engine handling early mount, signals, and Stage 1, 2, 3 phases.
* `src/fasdscan.cpp` — Asynchronous service directory scanner that watches `/var/service/`.
* `src/fasdsupervisor.cpp` — Monolithic individual process controller with binary `TAI64` status reporting.
* `src/fasdctl.cpp` — High-efficiency CLI supervisor controller and plain-text log reader.
* `Makefile` — Native GNU build file optimized for machine-specific execution (`-O3 -march=native`).

### 📚 Documentation & Guides
Detailed installation guides, real-hardware deployment instructions, and stage script templates can be found in our official project documentation:
👉 **[Read the fasdinit Wiki (Coming Soon)](#)**

---

## Русский

**fasdinit** — это современная, модульная и молниеносная система инициализации и супервизор сервисов, написанные с нуля на C++20. Спроектированная как высокопроизводительная и предсказуемая альтернатива `systemd` и `dinit`, она привносит архитектуру высокой конкурентности (high-concurrency) на самый ранний этап загрузки Linux, гарантируя нулевой оверхед и абсолютную надежность.

Сочетая легковесную структуру каталогов в стиле `runit` с современными примитивами ядра Linux (`signalfd`, неблокирующие циклы `select`, cgroups v2), **fasdinit** полностью устраняет задержки интерпретаторов командной строки при старте ОС, обеспечивая практически мгновенный запуск рабочего стола.

### ⚡ Ключевые особенности архитектуры
* **Честный параллельный планировщик:** Все независимые службы и подсистемы инициализации оборудования запускаются в пуле потоков строго одновременно, утилизируя все ядра твоего процессора.
* **Микросекундный байтовый IPC (`fasdctl`):** Управление службами происходит путем отправки всего одного сырого байта напрямую в неблокирующие FIFO-пайпы ядра Linux.
* **Прямое текстовое логирование:** Супервизор автоматически перехватывает дескрипторы процессов служб и пишет их в простые человекочитаемые файлы в `/var/log/fasdinit/` без использования тяжелых бинарных баз данных.
* **Адаптивный каскад графики и TTY:** Система сама сканирует дистрибутив на наличие экранных менеджеров (`sddm`, `lightdm`, `gdm`, `lxdm`). Если графика отсутствует, поток мгновенно и без зависаний переключается на открытие интерактивной консоли.
* **Родная интеграция с cgroups v2:** Процесс PID 1 автоматически разворачивает унифицированную иерархию контрольных групп и изолирует системные процессы внутри `system.slice` для тотального контроля ресурсов.

### 📂 Структура репозитория
* `src/fasdinit.cpp` — Ядро PID 1, отвечающее за раннее монтирование, cgroups, сигналы и стадии загрузки.
* `src/fasdscan.cpp` — Асинхронный сканер директории `/var/service/` на базе системных вызовов POSIX.
* `src/fasdsupervisor.cpp` — Супервизор конкретного процесса, побайтово совместимый с бинарным статусом `TAI64`.
* `src/fasdctl.cpp` — Революционный консольный менеджер управления и встроенный читальщик текстовых логов.
* `Makefile` — Скрипт сборки, оптимизированный под инструкции именно твоего процессора (`-O3 -march=native`).

### 📚 Документация и руководства
Подробные инструкции по интеграции в собственный дистрибутив, примеры скриптов стадий и руководства по развертыванию на реальном железе доступны в нашей базе знаний:
👉 **[Перейти в fasdinit Wiki (Ссылка появится позже)](#)**
