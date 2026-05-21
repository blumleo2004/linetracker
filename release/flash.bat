@echo off
echo ===================================================
echo   Linetracker Firmware Flasher (ESP32-S3)
echo ===================================================
echo.
echo Stelle sicher, dass Python installiert ist und das Display per USB verbunden ist!
echo.
pause

echo Installiere esptool...
pip install esptool
echo.

echo Starte Flash-Vorgang...
python -m esptool --chip esp32s3 --baud 460800 --before default_reset --after hard_reset write_flash -z --flash_mode dio --flash_freq 80m --flash_size 16MB 0x0000 bootloader.bin 0x8000 partitions.bin 0xe000 boot_app0.bin 0x10000 firmware.bin

echo.
echo ===================================================
echo   Fertig! Das Display sollte nun neu starten.
echo ===================================================
pause
