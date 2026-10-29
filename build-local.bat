@echo off
echo ========================================================
echo  Compiling Xiaomi Lamp 1S Matter Firmware via Local WSL
echo  Hardware: Intel Core Ultra 7 155H (22 threads)
echo ========================================================

python -c "p='version.txt'; v=open(p).read().strip() if __import__('os').path.exists(p) else '1.0.0'; print(f'=== Building Firmware Version {v} ===')"

wsl -e bash -c "sudo service docker status >/dev/null 2>&1 || sudo service docker start >/dev/null 2>&1"

wsl -e bash -c "sudo docker run --rm -v '/mnt/c/Users/dizir/Desktop/Xiaomi Lamp 1S MoW project':/project -w /project espressif/esp-matter:latest bash -c 'set -e; git config --global --add safe.directory /project; if [ -f /opt/espressif/esp-idf/export.sh ]; then . /opt/espressif/esp-idf/export.sh; export ESP_MATTER_PATH=/opt/espressif/esp-matter; else . /opt/esp/idf/export.sh; export ESP_MATTER_PATH=/opt/esp/esp-matter; fi; echo === Building with all CPU cores ===; idf.py -C /project build; cd /project/build; esptool.py --chip esp32 merge_bin -o /project/merged-flash.bin @flash_args; echo === Local Build Complete! merged-flash.bin updated ==='"

if %ERRORLEVEL% EQU 0 (
    echo.
    echo ========================================================
    echo  SUCCESS! merged-flash.bin is ready to flash.
    echo ========================================================
) else (
    echo.
    echo [ERROR] Build failed with exit code %ERRORLEVEL%
)
