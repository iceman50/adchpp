"""Cross-platform ADCH++ configuration manager."""

from .model import ConfigError, ConfigModel, CoreSettings, ListenerSettings

__all__ = ["ConfigError", "ConfigModel", "CoreSettings", "ListenerSettings"]
__version__ = "0.1.0"
