"""Host preferences, written atomically beside the units' saved memories."""

import json
import os
import tempfile
from dataclasses import asdict, dataclass, field
from gettext import gettext as _
from pathlib import Path

from profiles import BY_KEY
from audio_options import AudioOptions


@dataclass
class Preferences:
    path: Path
    quick_keys: bool = False
    unit: str = "bl-en"
    audio: AudioOptions = field(default_factory=AudioOptions)

    @classmethod
    def load(cls, path: Path) -> "Preferences":
        try:
            data = json.loads(path.read_text(encoding="utf-8"))
        except FileNotFoundError:
            return cls(path)
        if not isinstance(data, dict) or not isinstance(data.get("quick_keys", False), bool):
            raise ValueError(_("Invalid emulator preferences."))
        unit = data.get("unit", "bl-en")
        if not isinstance(unit, str) or unit not in BY_KEY:
            raise ValueError(_("Invalid emulator firmware preference."))
        return cls(path, data.get("quick_keys", False), unit, AudioOptions.load(data.get("audio", {})))

    def store(self, quick_keys: bool, *, unit: str | None = None, audio: AudioOptions | None = None) -> None:
        selected = self.unit if unit is None else unit
        sound = self.audio if audio is None else audio
        with tempfile.NamedTemporaryFile(mode="w", encoding="utf-8", dir=self.path.parent,
                                         prefix=self.path.name + ".", delete=False) as stream:
            tmp = Path(stream.name)
            try:
                json.dump({"quick_keys": quick_keys, "unit": selected, "audio": asdict(sound)}, stream)
                stream.write("\n")
                stream.flush()
                os.fsync(stream.fileno())
                os.replace(tmp, self.path)
            finally:
                tmp.unlink(missing_ok=True)
        self.quick_keys = quick_keys
        self.unit = selected
        self.audio = sound
