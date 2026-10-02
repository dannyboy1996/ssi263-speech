"""Persisted audio controls corresponding to the shared emulator's sound menu."""

from dataclasses import asdict, dataclass
from gettext import gettext as _

RATES = (11025, 16000, 22050, 32000, 44100, 48000)
BUFFERS = ("auto", "medium", "long")


@dataclass(frozen=True)
class AudioOptions:
    rate: int = 22050
    buffer: str = "long"
    idle: int = 3
    keep_open: int = 1
    pop_click: bool = True
    tick: bool = True

    @classmethod
    def load(cls, data: object) -> "AudioOptions":
        if not isinstance(data, dict) or data.keys() - asdict(cls()).keys():
            raise ValueError(_("Invalid emulator audio preferences."))
        options = cls(**data)
        if (type(options.rate) is not int or options.rate not in RATES
                or not isinstance(options.buffer, str) or options.buffer not in BUFFERS
                or type(options.idle) is not int or options.idle not in range(4)
                or type(options.keep_open) is not int or options.keep_open not in range(3)
                or type(options.pop_click) is not bool or type(options.tick) is not bool):
            raise ValueError(_("Invalid emulator audio preferences."))
        return options

    def command(self) -> str:
        return "AUDIO {rate} {buffer} {idle} {keep} {pop} {tick}".format(
            rate=self.rate, buffer=self.buffer, idle=self.idle, keep=self.keep_open,
            pop=int(self.pop_click), tick=int(self.tick))
