"""Configuration loaded from environment variables with sensible defaults."""

import os


class Settings:
    """Application settings, populated from environment variables."""

    def __init__(self) -> None:
        self.MQTT_BROKER: str = os.getenv("MQTT_BROKER", "localhost")
        self.MQTT_PORT: int = int(os.getenv("MQTT_PORT", "1883"))
        self.DB_PATH: str = os.getenv("DB_PATH", "zymoscope.db")
        self.HOST: str = os.getenv("HOST", "0.0.0.0")
        self.PORT: int = int(os.getenv("PORT", "8000"))
        # Kasa smart plug IPs (optional — leave empty to use GPIO relays).
        # Optional matching alias used as a fallback when the IP changes
        # (DHCP lease shuffles, plug reboots). If set, the dashboard
        # broadcast-discovers and rebinds to the device with this alias.
        self.KASA_HEATER_HOST: str = os.getenv("KASA_HEATER_HOST", "")
        self.KASA_HEATER_ALIAS: str = os.getenv("KASA_HEATER_ALIAS", "")
        self.KASA_COOLER_HOST: str = os.getenv("KASA_COOLER_HOST", "")
        self.KASA_COOLER_ALIAS: str = os.getenv("KASA_COOLER_ALIAS", "")
        # Switch the plugs off if no telemetry arrives for this many seconds
        # (ESP32 publishes every 30 s). 0 disables the watchdog.
        self.PLUG_WATCHDOG_SEC: int = int(os.getenv("PLUG_WATCHDOG_SEC", "180"))
        # Phone alerts via ntfy (see alerts.py). Both empty = disabled.
        self.NTFY_URL: str = os.getenv("NTFY_URL", "")          # e.g. http://localhost:2586
        self.NTFY_TOPIC: str = os.getenv("NTFY_TOPIC", "")
        # Tapping a notification opens this (e.g. the dashboard's Tailscale URL).
        self.ALERT_CLICK_URL: str = os.getenv("ALERT_CLICK_URL", "")
        self.ALERT_OFFLINE_SEC: int = int(os.getenv("ALERT_OFFLINE_SEC", "180"))
        self.ALERT_RANGE_C: float = float(os.getenv("ALERT_RANGE_C", "3"))
        self.ALERT_RANGE_MIN: float = float(os.getenv("ALERT_RANGE_MIN", "15"))


settings = Settings()
