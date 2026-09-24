# Windows 11 x64: сборка, установка и настройка

## Сборка

Windows 11 AMD64, PowerShell 7, CMake ≥3.20, Ninja и LLVM-MinGW x64/UCRT.
Проверяемый toolchain — LLVM-MinGW 20260616 / Clang 22.1.8. Добавьте CMake/Ninja
в PATH. Отдельно получите ASIO SDK на [сайте Steinberg](https://www.steinberg.net/developers/).
В `ASIO_SDK_DIR` должны находиться `common/asio.h` и `common/iasiodrv.h`.

## Зависимость AoIP-lib

`external/AoIP-lib` — git submodule, закреплённый конкретным commit. После обычного
clone выполните `git submodule update --init --recursive`. Для приватных
репозиториев Git должен иметь доступ и к основному проекту, и к AoIP-lib.
Токены и пароли в CMake или `.gitmodules` не сохраняются.

Вместо submodule можно передать `-DAOIP_SOURCE_DIR=/path/to/AoIP-lib` либо
установить SDK AoIP-lib и передать `-DCMAKE_PREFIX_PATH=/path/to/sdk`.
Приоритет: явный source path → submodule → установленный пакет.
Чтобы явно использовать установленный SDK, не инициализируйте submodule.
Обновление зависимости: выберите проверенный commit внутри submodule и
закоммитьте новый gitlink в родительском репозитории.

Сборка рабочего драйвера напрямую через CMake:

```powershell
cmake -S . -B build/windows -G Ninja -DCMAKE_BUILD_TYPE=Release `
  -DCMAKE_CXX_COMPILER=C:/Tools/llvm-mingw/bin/x86_64-w64-mingw32-clang++.exe `
  -DCMAKE_RC_COMPILER=C:/Tools/llvm-mingw/bin/x86_64-w64-mingw32-windres.exe `
  -DASIO_SDK_DIR=C:/SDK/asio
cmake --build build/windows --parallel 2
```

Готовые `PiAoipAsio.dll` и `PiAoipControl.exe` находятся
в `build/windows/bin`. Нативные зависимости приложения — системные DLL Windows;
C++ runtime для MinGW включён статически. Сборка не меняет системную регистрацию.

## MSI

Скрипты сборки и исходники установщика находятся в закрытом
[AoIP-debug-tool](https://github.com/danrey-bilo/AoIP-debug-tool/blob/main/docs/BUILD.md)
для разработчиков проекта. Для основной CMake-сборки доступ к нему не нужен.
Упаковка требует WiX Toolset 7 и расширений UI/Firewall. MSI содержит DLL,
панель настроек, инструкции и лицензии; тестовый ASIO-хост в него не входит.
Для установки используйте полученный `PiAoIP-2.1.0-Windows11-x64.msi`.
Цифровая подпись требует сертификата издателя. Условия распространения ASIO-сборки
описаны в [ASIO-SDK.md](ASIO-SDK.md).

## Установка

1. Закройте ASIO-хосты и запустите MSI, подтвердив UAC.
2. Откройте **PiAoIP Settings** из меню «Пуск».
3. Найдите Pi через **Discover Pi** или задайте его Ethernet IPv4.
4. Выберите в DAW драйвер **Pi AoIP** и нужные входы/выходы.

MSI устанавливает файлы в Program Files, регистрирует ASIO/COM и создаёт правило
UDP 50021 для LocalSubnet. `regsvr32`, INF, тестовый режим Windows и kernel driver
для этой DLL не используются. Windows ARM64/Server и Windows 10 не являются
целевыми платформами установщика.

## Сеть и профиль

Нужен Gigabit Ethernet full duplex, MTU 1500. Пример выделенной подсети:
Windows `192.168.50.1/24`, Pi `192.168.50.2/24`. На Pi укажите адрес Windows через
`piaoip-configure`. Интернет может идти по другому интерфейсу.

Начните с 64 входов / 64 выходов, 192 кГц, PCM32, ASIO64, guard512. Это проверенный
на одном стенде профиль, а не универсальная гарантия. Уменьшайте guard/буфер по одному
и проверяйте missing, late, deadline, overflow, TX expired и RTT. 64×192 кГц/PCM32
требуют примерно 426 Мбит/с в каждом направлении с заголовками.

INI хранится в `%LOCALAPPDATA%/PiAoIP/PiAoipAsio.ini`. Существующий переносимый
INI рядом с DLL имеет приоритет; `PIAOIP_CONFIG_PATH` задаёт явный путь для тестов.
Unicode-пути поддерживаются. Установщик может перенести прежние настройки;
для изменения пользовательского INI права администратора не нужны.

**Measure RTT** временно использует канал 32. Нужен цифровой маршрут input32 →
output32 без эффектов, минимум 32 входа/выхода и PCM32. Это цифровой round trip,
а не аналоговая задержка или отдельно измеренная задержка входа/выхода.

## Обновление и удаление

Закройте DAW перед обновлением. Удаление: «Параметры → Приложения → PiAoIP».
MSI удаляет собственные файлы, регистрацию и firewall-правило; пользовательский
INI сохраняется. Повторный запуск MSI позволяет восстановить компоненты.
Системный WDM/WASAPI endpoint не создаётся: Discord и Telegram напрямую ASIO DLL
не используют. [API и свой хост](API.md).

## Проверки разработчика

Тесты конфигурации и `smoke_host` перенесены в
[AoIP-debug-tool](https://github.com/danrey-bilo/AoIP-debug-tool/blob/main/docs/windows/TESTING.md).
Рабочий драйвер и его CMake-сборка от этого репозитория не зависят.
