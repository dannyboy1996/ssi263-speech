"""The existing emulator's six-dot firmware profiles and separate saved memories."""

from dataclasses import dataclass
from gettext import gettext as _
from pathlib import Path


@dataclass(frozen=True)
class Profile:
    key: str
    label: str
    firmware: tuple[str, ...]
    factory: tuple[str, ...]
    saved: str
    kind: str = "bl"

    def paths(self, firmware_dir: Path, state_dir: Path) -> tuple[Path, Path | None, Path]:
        def find(names: tuple[str, ...]) -> Path:
            return next((firmware_dir / name for name in names if (firmware_dir / name).is_file()), firmware_dir / names[0])
        return find(self.firmware), find(self.factory) if self.factory else None, state_dir / self.saved

    def available(self, firmware_dir: Path, state_dir: Path) -> bool:
        firmware, factory, saved = self.paths(firmware_dir, state_dir)
        return firmware.is_file() and (factory is None or factory.is_file() or saved.is_file())


# Same filenames and layouts as src/apps/blazie/main_linux.c KINDS.
# Old Braille Lite 18/40 images are not supported by the existing core.
PROFILES = (
    Profile("bl-en", _("Braille Lite 2000 (English)"), ("BL2ENG.BNS",), ("bl2_2003_warm.state",), "english.state"),
    Profile("bl-es", _("Braille Lite 2000 (Spanish)"), ("spanish/BL2SPA.BNS", "BL2SPA.BNS"),
            ("spanish/bl2spa_fresh.state", "bl2spa_fresh.state"), "spanish.state"),
    Profile("tns-en", _("Type 'n Speak (English, speech only)"), ("tns/TNSENG.TNS", "TNSENG.TNS"),
            (), "tns_english.state", "tns"),
    Profile("tns-es", _("Type 'n Speak (Spanish, speech only)"), ("tns/TNSSPA.TNS", "TNSSPA.TNS"),
            (), "tns_spanish.state", "tns"),
    Profile("bns-en", _("Braille 'n Speak 2000 (English, speech only)"), ("bns2000/BS03ENG.BNS", "BS03ENG.BNS"),
            ("bns2000/bs03eng_fresh.state", "bs03eng_fresh.state"), "bns_english.state"),
    Profile("bns-sk", _("Braille 'n Speak 2000 (Slovak, speech only)"), ("bns2000/BS2SLL.BNS", "BS2SLL.BNS"),
            ("bns2000/bs2sll_fresh.state", "bs2sll_fresh.state"), "bns_slovak.state"),
)
BY_KEY = {profile.key: profile for profile in PROFILES}
