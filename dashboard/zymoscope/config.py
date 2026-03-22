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
        # Kasa smart plug IPs (optional — leave empty to use GPIO relays)
        self.KASA_HEATER_HOST: str = os.getenv("KASA_HEATER_HOST", "")
        self.KASA_COOLER_HOST: str = os.getenv("KASA_COOLER_HOST", "")


settings = Settings()
