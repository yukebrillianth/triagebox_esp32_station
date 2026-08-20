@echo off
rem Bench broker bring-up for the TriageBox station. Run from the repo root:
rem
rem     tools\bench-broker.cmd
rem
rem Checks the three things that each make the station log an identical
rem "transport connect timeout" -- wrong adapter IP, blocked port, broker not
rem running -- then starts Mosquitto in the foreground. Ctrl-C stops it.
rem
rem The checks exist because the failure they catch is invisible from the
rem station's end: esp-tls reports "select() timeout" whether nothing is
rem listening, the firewall dropped the SYN, or the PC is not on this subnet.
setlocal

set BROKER_IP=192.168.50.1
set BROKER_PORT=1883
set MOSQ="C:\Program Files\mosquitto\mosquitto.exe"

echo === 1/3  adapter %BROKER_IP% ===
rem Get-NetIPAddress and not `ipconfig ^| findstr`: findstr would match
rem 192.168.50.150 as a substring of the address we want and pass an adapter
rem the station cannot reach.
rem if/else rather than the ?: ternary: that operator is PowerShell 7 only and
rem Windows ships 5.1, where it is a parse error -- which would exit non-zero and
rem look exactly like a missing adapter.
powershell -NoProfile -Command "if (Get-NetIPAddress -IPAddress '%BROKER_IP%' -ErrorAction SilentlyContinue) { exit 0 } else { exit 1 }"
if errorlevel 1 (
    echo   MISSING. No adapter on this PC holds %BROKER_IP%.
    echo   Set the direct-cable adapter to a static %BROKER_IP% / 255.255.255.0,
    echo   no gateway. The station is configured for exactly this address
    echo   ^(CONFIG_TB_MQTT_URI in sdkconfig.defaults^), so nothing else works.
    echo   Adapter names:
    netsh interface ipv4 show interfaces
    goto :fail
)
echo   ok

echo === 2/3  inbound TCP %BROKER_PORT% ===
netsh advfirewall firewall show rule name="TriageBox MQTT 1883" >nul 2>&1
if errorlevel 1 (
    echo   No firewall rule yet. Adding one ^(needs Administrator^)...
    netsh advfirewall firewall add rule name="TriageBox MQTT 1883" dir=in action=allow protocol=TCP localport=%BROKER_PORT% >nul 2>&1
    if errorlevel 1 (
        echo   FAILED -- not elevated. Either re-run this script as
        echo   Administrator, or run that one command in an admin prompt:
        echo.
        echo     netsh advfirewall firewall add rule name="TriageBox MQTT 1883" dir=in action=allow protocol=TCP localport=%BROKER_PORT%
        echo.
        echo   Without it Windows drops the station's SYN silently and the
        echo   station logs a connect timeout that looks like a dead cable.
        goto :fail
    )
    echo   added
) else (
    echo   ok
)

echo === 3/3  mosquitto ===
if not exist %MOSQ% (
    echo   Not at %MOSQ%.
    echo   Edit MOSQ at the top of this script to wherever it installed.
    goto :fail
)
rem The service and this foreground instance would fight over the port, and the
rem service runs with the DEFAULT config -- 127.0.0.1 only -- so the station
rem still cannot reach it. Foreground with -v is what makes a bench debuggable.
sc query mosquitto 2>nul | findstr /c:"RUNNING" >nul
if not errorlevel 1 (
    echo   The mosquitto SERVICE is running and holds port %BROKER_PORT%.
    echo   It uses the default config ^(loopback only^), so stop it first:
    echo     net stop mosquitto
    goto :fail
)

echo   starting on %BROKER_IP%:%BROKER_PORT%, Ctrl-C to stop
echo.
echo   In a second window, watch everything the station publishes:
echo     "C:\Program Files\mosquitto\mosquitto_sub.exe" -h %BROKER_IP% -t "triagebox/#" -v
echo.
%MOSQ% -c "%~dp0mosquitto.conf" -v
goto :eof

:fail
echo.
echo Not started. Fix the above and re-run.
exit /b 1
