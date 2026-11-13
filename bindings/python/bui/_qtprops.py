"""Qt properties, declared: one line per property gives a class Qt's accessors.

    @qt_properties(statusTip=QtProperty(default=""),
                   suffix=QtProperty(kit="suffix", text=True),
                   flat=QtProperty(default=False, getter="isFlat"))

gives ``statusTip()`` / ``setStatusTip(v)``, ``suffix()`` / ``setSuffix(v)`` (stored on the kit
node, so the widget shows it) and ``isFlat()`` / ``setFlat(v)``. A property without ``kit`` is
state Qt itself only stores and hands back (``statusTip``, ``whatsThis``, ``accessibleName``): it
has no effect here either. ``changed`` names a signal of the class, emitted when the value changes.
A method the class defines itself is never replaced.

SPDX-License-Identifier: GPL-2.0-or-later
"""


class QtProperty:
    __slots__ = ("kit", "default", "to_kit", "from_kit", "getter", "changed", "text")

    def __init__(self, kit=None, default=None, to_kit=None, from_kit=None, getter=None, changed=None,
                 text=False):
        self.kit, self.default, self.to_kit, self.from_kit = kit, default, to_kit, from_kit
        self.getter, self.changed, self.text = getter, changed, text


def qt_properties(**props):
    """Class decorator: Qt's getter and setter for each property."""
    def apply(cls):
        for name, p in props.items():
            _install(cls, name, p)
        return cls
    return apply


def _install(cls, name, p):
    def read(self):
        if p.kit is None:
            return self.__dict__.get("_qp", {}).get(name, p.default)
        v = self.get_str(p.kit) if p.text else self.get(p.kit)
        return p.from_kit(v) if p.from_kit else v

    def write(self, value):
        old = read(self)
        if p.kit is None:
            self.__dict__.setdefault("_qp", {})[name] = value
        else:
            self.set(p.kit, p.to_kit(value) if p.to_kit else value)
        if p.changed and old != value:
            getattr(self, p.changed).emit(read(self))

    getter = p.getter or name
    setter = "set" + name[0].upper() + name[1:]
    read.__name__, write.__name__ = getter, setter
    for attr, fn in ((getter, read), (setter, write)):
        if attr not in cls.__dict__:
            setattr(cls, attr, fn)
