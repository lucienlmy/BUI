"""Dates and times on BUI: QDate, QTime and QDateTime (Python's datetime underneath, Qt's formats on
top), QDateTimeEdit / QDateEdit / QTimeEdit (one Blender number field per section of the display
format, in an aligned row like Blender's vector fields; named months and AM/PM are dropdowns) and
QCalendarWidget (a month of day buttons under Blender's header row).

SPDX-License-Identifier: GPL-2.0-or-later
"""

import calendar
import datetime as _dt
import re

import buipy

from bui.qtsyntax import (QAbstractSpinBox, QComboBox, QGridLayout, QHBoxLayout, QIcon, QLabel, QPushButton,
                          QSpinBox, QVBoxLayout, QWidget, Qt, Signal)

_MONTHS = [calendar.month_name[i] for i in range(1, 13)]
_DAYS = [calendar.day_name[i] for i in range(7)]  # Monday first, as Qt's 1..7
# Qt's format tokens, longest first: the date's and the time's
_TOKENS = re.compile(r"'[^']*'|dddd|ddd|dd|d|MMMM|MMM|MM|M|yyyy|yy|HH|H|hh|h|mm|m|ss|s|zzz|z|AP|ap|A|a")


def _format(fmt, date=None, time=None):
    """`fmt` with Qt's tokens filled in from a date and a time (Qt's QLocale::toString formats)."""
    twelve = bool(re.search(r"AP|ap|A|a", fmt))

    def token(m):
        t = m.group(0)
        if t.startswith("'"):
            return t[1:-1] or "'"
        if date is not None and t[0] in "dMy":
            return {"d": str(date.day()), "dd": "%02d" % date.day(), "ddd": _DAYS[date.dayOfWeek() - 1][:3],
                    "dddd": _DAYS[date.dayOfWeek() - 1], "M": str(date.month()), "MM": "%02d" % date.month(),
                    "MMM": _MONTHS[date.month() - 1][:3], "MMMM": _MONTHS[date.month() - 1],
                    "yy": "%02d" % (date.year() % 100), "yyyy": "%04d" % date.year()}[t]
        if time is not None:
            h = time.hour()
            h12 = (h % 12) or 12
            return {"H": str(h), "HH": "%02d" % h, "h": str(h12 if twelve else h),
                    "hh": "%02d" % (h12 if twelve else h), "m": str(time.minute()),
                    "mm": "%02d" % time.minute(), "s": str(time.second()), "ss": "%02d" % time.second(),
                    "z": str(time.msec()), "zzz": "%03d" % time.msec(), "AP": "PM" if h >= 12 else "AM",
                    "A": "PM" if h >= 12 else "AM", "ap": "pm" if h >= 12 else "am",
                    "a": "pm" if h >= 12 else "am"}.get(t, t)
        return t
    return _TOKENS.sub(token, fmt)


def _parse(text, fmt):
    """The fields Qt's fromString(text, format) reads: {token kind: value}; None when it doesn't match."""
    pattern, kinds = "", []
    pos = 0
    for m in _TOKENS.finditer(fmt):
        pattern += re.escape(fmt[pos:m.start()])
        t = m.group(0)
        if t.startswith("'"):
            pattern += re.escape(t[1:-1])
        elif t in ("MMM", "MMMM", "ddd", "dddd"):
            pattern += r"([A-Za-z]+)"
            kinds.append(t)
        elif t in ("AP", "ap", "A", "a"):
            pattern += r"([AaPp][Mm])"
            kinds.append("ap")
        else:
            pattern += r"(\d{%d})" % len(t) if len(t) > 1 and t != "zzz" else r"(\d+)"
            kinds.append(t[0] if t != "yy" else "yy")
        pos = m.end()
    pattern += re.escape(fmt[pos:])
    m = re.fullmatch(pattern, str(text).strip())
    if not m:
        return None
    out = {}
    for kind, value in zip(kinds, m.groups()):
        if kind in ("MMM", "MMMM"):
            names = [n.lower() for n in _MONTHS]
            short = [n[:3] for n in names]
            v = value.lower()
            if v not in names and v not in short:
                return None
            out["M"] = (names.index(v) if v in names else short.index(v)) + 1
        elif kind in ("ddd", "dddd"):
            continue
        elif kind == "ap":
            out["ap"] = value.lower()
        elif kind == "yy":
            out["y"] = 1900 + int(value) if int(value) >= 50 else 2000 + int(value)  # Qt reads yy as 19xx
        else:
            out[kind.lower() if kind in "Hh" else kind] = int(value)
    if "ap" in out and "h" in out:
        out["h"] = out["h"] % 12 + (12 if out["ap"] == "pm" else 0)
    return out


# ==========================================================================
# Values


class QDate:
    """A day on the proleptic Gregorian calendar; QDate() is null (invalid)."""

    def __init__(self, *args):
        self._d = None
        if len(args) == 1 and isinstance(args[0], _dt.date):
            self._d = _dt.date(args[0].year, args[0].month, args[0].day)
        elif len(args) >= 3:
            try:
                self._d = _dt.date(int(args[0]), int(args[1]), int(args[2]))
            except ValueError:
                self._d = None

    @staticmethod
    def currentDate():  # noqa: N802
        return QDate(_dt.date.today())

    @staticmethod
    def isLeapYear(year):  # noqa: N802
        return calendar.isleap(int(year))

    @staticmethod
    def fromString(text, fmt=Qt.TextDate):  # noqa: N802
        if fmt == Qt.ISODate:
            fmt = "yyyy-MM-dd"
        elif not isinstance(fmt, str):
            fmt = "ddd MMM d yyyy"
        f = _parse(text, fmt)
        return QDate(f.get("y", 1900), f.get("M", 1), f.get("d", 1)) if f is not None else QDate()

    @staticmethod
    def fromJulianDay(jd):  # noqa: N802
        return QDate(_dt.date.fromordinal(int(jd) - 1721425))

    def isValid(self):  # noqa: N802
        return self._d is not None

    def isNull(self):  # noqa: N802
        return self._d is None

    def year(self):
        return self._d.year if self._d else 0

    def month(self):
        return self._d.month if self._d else 0

    def day(self):
        return self._d.day if self._d else 0

    def dayOfWeek(self):  # noqa: N802
        return self._d.isoweekday() if self._d else 0

    def dayOfYear(self):  # noqa: N802
        return self._d.timetuple().tm_yday if self._d else 0

    def daysInMonth(self):  # noqa: N802
        return calendar.monthrange(self._d.year, self._d.month)[1] if self._d else 0

    def daysInYear(self):  # noqa: N802
        return (366 if calendar.isleap(self._d.year) else 365) if self._d else 0

    def weekNumber(self):  # noqa: N802
        return self._d.isocalendar()[1] if self._d else 0

    def toJulianDay(self):  # noqa: N802
        return self._d.toordinal() + 1721425 if self._d else 0

    def setDate(self, y, m, d):  # noqa: N802
        self.__init__(y, m, d)
        return self.isValid()

    def getDate(self):  # noqa: N802
        return self.year(), self.month(), self.day()

    def addDays(self, n):  # noqa: N802
        return QDate(self._d + _dt.timedelta(days=int(n))) if self._d else QDate()

    def addMonths(self, n):  # noqa: N802
        """The same day n months on, or the month's last day when it is shorter (Qt)."""
        if not self._d:
            return QDate()
        months = self._d.year * 12 + self._d.month - 1 + int(n)
        y, m = divmod(months, 12)
        return QDate(y, m + 1, min(self._d.day, calendar.monthrange(y, m + 1)[1]))

    def addYears(self, n):  # noqa: N802
        return self.addMonths(12 * int(n))

    def daysTo(self, other):  # noqa: N802
        return (other._d - self._d).days if self._d and other._d else 0

    def toString(self, fmt=Qt.TextDate):  # noqa: N802
        if not self._d:
            return ""
        if fmt == Qt.ISODate:
            return self._d.isoformat()
        return _format(fmt if isinstance(fmt, str) else "ddd MMM d yyyy", date=self)

    def toPython(self):  # noqa: N802
        return self._d

    toPyDate = toPython

    def _key(self):
        return self._d or _dt.date.min

    def __eq__(self, other):
        return isinstance(other, QDate) and self._d == other._d

    def __lt__(self, other):
        return self._key() < other._key()

    def __le__(self, other):
        return self._key() <= other._key()

    def __gt__(self, other):
        return self._key() > other._key()

    def __ge__(self, other):
        return self._key() >= other._key()

    def __hash__(self):
        return hash(self._d)

    def __repr__(self):
        return "QDate(%d, %d, %d)" % self.getDate() if self._d else "QDate()"


class QTime:
    """A time of day to the millisecond; QTime() is null (invalid)."""

    def __init__(self, *args):
        self._ms = None  # milliseconds since midnight
        if len(args) == 1 and isinstance(args[0], _dt.time):
            t = args[0]
            self._ms = ((t.hour * 60 + t.minute) * 60 + t.second) * 1000 + t.microsecond // 1000
        elif len(args) >= 2:
            h, m, s, ms = (list(args) + [0, 0])[:4]
            if 0 <= h < 24 and 0 <= m < 60 and 0 <= s < 60 and 0 <= ms < 1000:
                self._ms = ((int(h) * 60 + int(m)) * 60 + int(s)) * 1000 + int(ms)

    @staticmethod
    def currentTime():  # noqa: N802
        return QTime(_dt.datetime.now().time())

    @staticmethod
    def fromString(text, fmt=Qt.TextDate):  # noqa: N802
        fmt = fmt if isinstance(fmt, str) else "HH:mm:ss"
        f = _parse(text, fmt)
        return QTime(f.get("h", 0), f.get("m", 0), f.get("s", 0), f.get("z", 0)) if f is not None else QTime()

    @staticmethod
    def fromMSecsSinceStartOfDay(ms):  # noqa: N802
        t = QTime()
        t._ms = int(ms) % 86400000
        return t

    def isValid(self):  # noqa: N802
        return self._ms is not None

    def isNull(self):  # noqa: N802
        return self._ms is None

    def hour(self):
        return self._ms // 3600000 if self._ms is not None else -1

    def minute(self):
        return self._ms // 60000 % 60 if self._ms is not None else -1

    def second(self):
        return self._ms // 1000 % 60 if self._ms is not None else -1

    def msec(self):
        return self._ms % 1000 if self._ms is not None else -1

    def msecsSinceStartOfDay(self):  # noqa: N802
        return self._ms or 0

    def setHMS(self, h, m, s, ms=0):  # noqa: N802
        self.__init__(h, m, s, ms)
        return self.isValid()

    def addMSecs(self, n):  # noqa: N802
        return QTime.fromMSecsSinceStartOfDay(self._ms + int(n)) if self._ms is not None else QTime()

    def addSecs(self, n):  # noqa: N802
        return self.addMSecs(int(n) * 1000)

    def msecsTo(self, other):  # noqa: N802
        return other._ms - self._ms if self._ms is not None and other._ms is not None else 0

    def secsTo(self, other):  # noqa: N802
        return int(self.msecsTo(other) / 1000)

    def toString(self, fmt=Qt.TextDate):  # noqa: N802
        if self._ms is None:
            return ""
        return _format(fmt if isinstance(fmt, str) else "HH:mm:ss", time=self)

    def toPython(self):  # noqa: N802
        return _dt.time(self.hour(), self.minute(), self.second(), self.msec() * 1000) if self.isValid() else None

    toPyTime = toPython

    def _key(self):
        return -1 if self._ms is None else self._ms

    def __eq__(self, other):
        return isinstance(other, QTime) and self._ms == other._ms

    def __lt__(self, other):
        return self._key() < other._key()

    def __le__(self, other):
        return self._key() <= other._key()

    def __gt__(self, other):
        return self._key() > other._key()

    def __ge__(self, other):
        return self._key() >= other._key()

    def __hash__(self):
        return hash(self._ms)

    def __repr__(self):
        return "QTime(%d, %d, %d, %d)" % (self.hour(), self.minute(), self.second(), self.msec()) \
            if self.isValid() else "QTime()"


class QDateTime:
    """A date and a time (local time)."""

    def __init__(self, *args):
        if len(args) == 1 and isinstance(args[0], _dt.datetime):
            self._date, self._time = QDate(args[0].date()), QTime(args[0].time())
        elif len(args) == 1 and isinstance(args[0], QDateTime):
            self._date, self._time = QDate(args[0]._date.toPython()), QTime.fromMSecsSinceStartOfDay(
                args[0]._time.msecsSinceStartOfDay())
        elif args and isinstance(args[0], QDate):
            self._date = args[0]
            self._time = args[1] if len(args) > 1 and isinstance(args[1], QTime) else QTime(0, 0)
        else:
            self._date, self._time = QDate(), QTime()

    @staticmethod
    def currentDateTime():  # noqa: N802
        return QDateTime(_dt.datetime.now())

    @staticmethod
    def currentSecsSinceEpoch():  # noqa: N802
        return int(_dt.datetime.now().timestamp())

    @staticmethod
    def currentMSecsSinceEpoch():  # noqa: N802
        return int(_dt.datetime.now().timestamp() * 1000)

    @staticmethod
    def fromSecsSinceEpoch(s):  # noqa: N802
        return QDateTime(_dt.datetime.fromtimestamp(int(s)))

    @staticmethod
    def fromMSecsSinceEpoch(ms):  # noqa: N802
        return QDateTime(_dt.datetime.fromtimestamp(int(ms) / 1000))

    @staticmethod
    def fromString(text, fmt=Qt.TextDate):  # noqa: N802
        if fmt == Qt.ISODate:
            fmt = "yyyy-MM-ddTHH:mm:ss"
        elif not isinstance(fmt, str):
            fmt = "ddd MMM d HH:mm:ss yyyy"
        f = _parse(text, fmt)
        if f is None:
            return QDateTime()
        return QDateTime(QDate(f.get("y", 1900), f.get("M", 1), f.get("d", 1)),
                         QTime(f.get("h", 0), f.get("m", 0), f.get("s", 0), f.get("z", 0)))

    def isValid(self):  # noqa: N802
        return self._date.isValid() and self._time.isValid()

    def isNull(self):  # noqa: N802
        return self._date.isNull() and self._time.isNull()

    def date(self):
        return self._date

    def time(self):
        return self._time

    def setDate(self, d):  # noqa: N802
        self._date = d

    def setTime(self, t):  # noqa: N802
        self._time = t

    def toPython(self):  # noqa: N802
        return _dt.datetime.combine(self._date.toPython(), self._time.toPython()) if self.isValid() else None

    toPyDateTime = toPython

    def toSecsSinceEpoch(self):  # noqa: N802
        return int(self.toPython().timestamp()) if self.isValid() else 0

    def toMSecsSinceEpoch(self):  # noqa: N802
        return int(self.toPython().timestamp() * 1000) if self.isValid() else 0

    def addMSecs(self, n):  # noqa: N802
        return QDateTime(self.toPython() + _dt.timedelta(milliseconds=int(n))) if self.isValid() else QDateTime()

    def addSecs(self, n):  # noqa: N802
        return self.addMSecs(int(n) * 1000)

    def addDays(self, n):  # noqa: N802
        return QDateTime(self._date.addDays(n), self._time)

    def addMonths(self, n):  # noqa: N802
        return QDateTime(self._date.addMonths(n), self._time)

    def addYears(self, n):  # noqa: N802
        return QDateTime(self._date.addYears(n), self._time)

    def secsTo(self, other):  # noqa: N802
        return int((other.toPython() - self.toPython()).total_seconds()) if self.isValid() and other.isValid() else 0

    def msecsTo(self, other):  # noqa: N802
        return int((other.toPython() - self.toPython()).total_seconds() * 1000) \
            if self.isValid() and other.isValid() else 0

    def daysTo(self, other):  # noqa: N802
        return self._date.daysTo(other._date)

    def toString(self, fmt=Qt.TextDate):  # noqa: N802
        if not self.isValid():
            return ""
        if fmt == Qt.ISODate:
            return self.toPython().isoformat(timespec="seconds")
        return _format(fmt if isinstance(fmt, str) else "ddd MMM d HH:mm:ss yyyy", self._date, self._time)

    def _key(self):
        return (self._date._key(), self._time._key())

    def __eq__(self, other):
        return isinstance(other, QDateTime) and self._key() == other._key()

    def __lt__(self, other):
        return self._key() < other._key()

    def __le__(self, other):
        return self._key() <= other._key()

    def __gt__(self, other):
        return self._key() > other._key()

    def __ge__(self, other):
        return self._key() >= other._key()

    def __hash__(self):
        return hash(self._key())

    def __repr__(self):
        return "QDateTime(%r, %r)" % (self._date, self._time)


# ==========================================================================
# Editors


class QDateTimeEdit(QAbstractSpinBox):
    """One Blender number field per section of the display format, in an aligned row (as Blender's
    vector fields); MMM / MMMM months and AM/PM are dropdowns."""
    KIND = "row"
    (NoSection, AmPmSection, MSecSection, SecondSection, MinuteSection, HourSection) = (0, 1, 2, 4, 8, 16)
    DaySection, MonthSection, YearSection = 0x100, 0x200, 0x400
    TimeSections_Mask = AmPmSection | MSecSection | SecondSection | MinuteSection | HourSection
    DateSections_Mask = DaySection | MonthSection | YearSection
    _SECTION = {"d": DaySection, "M": MonthSection, "y": YearSection, "H": HourSection, "h": HourSection,
                "m": MinuteSection, "s": SecondSection, "z": MSecSection, "A": AmPmSection, "a": AmPmSection}
    _FORMAT = "yyyy-MM-dd HH:mm"
    dateTimeChanged = Signal(object)
    dateChanged = Signal(object)
    timeChanged = Signal(object)
    userDateChanged = Signal(object)

    def __init__(self, value=None, parent=None):
        if isinstance(value, QWidget):
            value, parent = None, value
        super().__init__(parent)
        self.set("align", True)  # the fields join, as Blender's vector fields do
        self._value = QDateTime(QDate(2000, 1, 1), QTime(0, 0))
        self._min = QDateTime(QDate(100, 1, 1), QTime(0, 0))
        self._max = QDateTime(QDate(9999, 12, 31), QTime(23, 59, 59, 999))
        self._fields = []  # (section, token, editor)
        self._current = 0
        self._updating = False
        self.setDisplayFormat(self._FORMAT)
        if isinstance(value, QDateTime):
            self.setDateTime(value)
        elif isinstance(value, QDate):
            self.setDate(value)
        elif isinstance(value, QTime):
            self.setTime(value)

    # -- the value -----------------------------------------------------------------------------------------
    def dateTime(self):  # noqa: N802
        return QDateTime(self._value)

    def date(self):
        return self._value.date()

    def time(self):
        return self._value.time()

    def setDateTime(self, dt):  # noqa: N802
        """Clamped to the range; dateTimeChanged (and dateChanged / timeChanged) when it changes."""
        dt = max(self._min, min(self._max, QDateTime(dt)))
        old = self._value
        if dt == old:
            return
        self._value = dt
        self._show()
        self.dateTimeChanged.emit(QDateTime(dt))
        if dt.date() != old.date():
            self.dateChanged.emit(dt.date())
            self.userDateChanged.emit(dt.date())
        if dt.time() != old.time():
            self.timeChanged.emit(dt.time())

    def setDate(self, d):  # noqa: N802
        self.setDateTime(QDateTime(d, self._value.time()))

    def setTime(self, t):  # noqa: N802
        self.setDateTime(QDateTime(self._value.date(), t))

    # -- the range -----------------------------------------------------------------------------------------
    def minimumDateTime(self):  # noqa: N802
        return QDateTime(self._min)

    def maximumDateTime(self):  # noqa: N802
        return QDateTime(self._max)

    def setDateTimeRange(self, lo, hi):  # noqa: N802
        self._min, self._max = QDateTime(lo), QDateTime(max(lo, hi))
        self.setDateTime(self._value)

    def setMinimumDateTime(self, dt):  # noqa: N802
        self.setDateTimeRange(dt, max(dt, self._max))

    def setMaximumDateTime(self, dt):  # noqa: N802
        self.setDateTimeRange(min(dt, self._min), dt)

    def clearMinimumDateTime(self):  # noqa: N802
        self.setMinimumDateTime(QDateTime(QDate(100, 1, 1), QTime(0, 0)))

    def clearMaximumDateTime(self):  # noqa: N802
        self.setMaximumDateTime(QDateTime(QDate(9999, 12, 31), QTime(23, 59, 59, 999)))

    def minimumDate(self):  # noqa: N802
        return self._min.date()

    def maximumDate(self):  # noqa: N802
        return self._max.date()

    def setDateRange(self, lo, hi):  # noqa: N802
        self.setDateTimeRange(QDateTime(lo, QTime(0, 0)), QDateTime(hi, QTime(23, 59, 59, 999)))

    def setMinimumDate(self, d):  # noqa: N802
        self.setMinimumDateTime(QDateTime(d, QTime(0, 0)))

    def setMaximumDate(self, d):  # noqa: N802
        self.setMaximumDateTime(QDateTime(d, QTime(23, 59, 59, 999)))

    def clearMinimumDate(self):  # noqa: N802
        self.clearMinimumDateTime()

    def clearMaximumDate(self):  # noqa: N802
        self.clearMaximumDateTime()

    def minimumTime(self):  # noqa: N802
        return self._min.time()

    def maximumTime(self):  # noqa: N802
        return self._max.time()

    def setTimeRange(self, lo, hi):  # noqa: N802
        d = self._value.date()
        self.setDateTimeRange(QDateTime(d, lo), QDateTime(d, hi))

    def setMinimumTime(self, t):  # noqa: N802
        self.setMinimumDateTime(QDateTime(self._value.date(), t))

    def setMaximumTime(self, t):  # noqa: N802
        self.setMaximumDateTime(QDateTime(self._value.date(), t))

    def clearMinimumTime(self):  # noqa: N802
        self.clearMinimumDateTime()

    def clearMaximumTime(self):  # noqa: N802
        self.clearMaximumDateTime()

    # -- the display format and its sections -----------------------------------------------------------------
    def displayFormat(self):  # noqa: N802
        return self._format

    def setDisplayFormat(self, fmt):  # noqa: N802
        """One field per date or time token: d M y (MMM and MMMM a month dropdown), H h m s z, AP."""
        self._format = str(fmt)
        for _, _, editor in self._fields:
            self.remove(editor)
        self._fields = []
        self._twelve = bool(re.search(r"AP|ap|A|a", self._format))
        for m in _TOKENS.finditer(self._format):
            t = m.group(0)
            if t.startswith("'") or t in ("ddd", "dddd"):
                continue
            self._fields.append((self._SECTION[t[0]], t, self._editor(t)))
        for _, _, editor in self._fields:
            self.add(editor)
        self._current = 0
        self._show()

    def _editor(self, t):
        if t in ("MMM", "MMMM"):
            e = QComboBox()
            e.addItems([n if t == "MMMM" else n[:3] for n in _MONTHS])
            e.currentIndexChanged.connect(lambda *_: self._from_fields())
            return e
        if t in ("AP", "ap", "A", "a"):
            e = QComboBox()
            e.addItems(["AM", "PM"] if t[0] == "A" else ["am", "pm"])
            e.currentIndexChanged.connect(lambda *_: self._from_fields())
            return e
        e = QSpinBox()
        lo, hi = {"d": (1, 31), "M": (1, 12), "y": (100, 9999), "H": (0, 23), "h": (1, 12) if self._twelve
                  else (0, 23), "m": (0, 59), "s": (0, 59), "z": (0, 999)}[t[0]]
        e.setRange(lo, hi)
        e.valueChanged.connect(lambda *_: self._from_fields())
        return e

    def displayedSections(self):  # noqa: N802
        out = 0
        for section, _, _ in self._fields:
            out |= section
        return out

    def sectionCount(self):  # noqa: N802
        return len(self._fields)

    def sectionAt(self, index):  # noqa: N802
        return self._fields[index][0] if 0 <= index < len(self._fields) else self.NoSection

    def sectionText(self, section):  # noqa: N802
        token = next((t for s, t, _ in self._fields if s == section), None)
        return _format(token, self.date(), self.time()) if token else ""

    def currentSection(self):  # noqa: N802
        return self.sectionAt(self._current)

    def setCurrentSection(self, section):  # noqa: N802
        i = next((k for k, (s, _, _) in enumerate(self._fields) if s == section), None)
        if i is not None:
            self._current = i

    def currentSectionIndex(self):  # noqa: N802
        return self._current

    def setCurrentSectionIndex(self, i):  # noqa: N802
        if 0 <= i < len(self._fields):
            self._current = int(i)

    # -- the fields and the value -----------------------------------------------------------------------------
    def _show(self):
        """The value in the fields (without their signals coming back)."""
        self._updating = True
        try:
            d, t = self._value.date(), self._value.time()
            for _, token, e in self._fields:
                k = token[0]
                if k in "Aa":
                    e.setCurrentIndex(1 if t.hour() >= 12 else 0)
                elif k == "M" and len(token) >= 3:
                    e.setCurrentIndex(d.month() - 1)
                else:
                    e.setValue({"d": d.day(), "M": d.month(), "y": d.year(), "H": t.hour(),
                                "h": (t.hour() % 12 or 12) if self._twelve else t.hour(), "m": t.minute(),
                                "s": t.second(), "z": t.msec()}[k])
        finally:
            self._updating = False

    def _from_fields(self):
        """The user changed a field: the new date and time (a day past the month's end is its last day)."""
        if self._updating:
            return
        d, t = self._value.date(), self._value.time()
        f = {"d": d.day(), "M": d.month(), "y": d.year(), "h": t.hour(), "m": t.minute(), "s": t.second(),
             "z": t.msec()}
        pm = t.hour() >= 12
        for _, token, e in self._fields:
            k = token[0]
            if k in "Aa":
                pm = e.currentIndex() == 1
            elif k == "M" and len(token) >= 3:
                f["M"] = e.currentIndex() + 1
            else:
                f["h" if k in "Hh" else k] = e.value()
        if self._twelve:
            f["h"] = f["h"] % 12 + (12 if pm else 0)
        f["d"] = min(f["d"], calendar.monthrange(f["y"], f["M"])[1])
        self.setDateTime(QDateTime(QDate(f["y"], f["M"], f["d"]), QTime(f["h"], f["m"], f["s"], f["z"])))
        self._show()  # a clamped value shows as it is
        self.editingFinished.emit()

    # -- QAbstractSpinBox ---------------------------------------------------------------------------------------
    def text(self):
        return self._value.toString(self._format)

    def validate(self, text, pos=0):
        from bui.qtgui import QValidator
        f = _parse(text, self._format)
        return (QValidator.Acceptable if f is not None else QValidator.Intermediate, text, pos)

    def stepBy(self, steps):  # noqa: N802
        """Steps the current section (Qt): days, months, years, hours, ..."""
        section = self.currentSection()
        v = self._value
        if section == self.DaySection:
            v = v.addDays(steps)
        elif section == self.MonthSection:
            v = v.addMonths(steps)
        elif section == self.YearSection:
            v = v.addYears(steps)
        elif section in (self.HourSection, self.AmPmSection):
            v = v.addSecs(3600 * steps * (12 if section == self.AmPmSection else 1))
        elif section == self.MinuteSection:
            v = v.addSecs(60 * steps)
        elif section == self.SecondSection:
            v = v.addSecs(steps)
        elif section == self.MSecSection:
            v = v.addMSecs(steps)
        self.setDateTime(v)

    def stepEnabled(self):  # noqa: N802
        if self.isReadOnly():
            return self.StepNone
        return ((self.StepUpEnabled if self._value < self._max else 0)
                | (self.StepDownEnabled if self._value > self._min else 0))

    def minimum(self):
        return self._min

    def maximum(self):
        return self._max

    def value(self):
        return self._value

    def setReadOnly(self, b):  # noqa: N802
        for _, _, e in self._fields:
            e.setEnabled(not b)
        self.__dict__["_read_only"] = bool(b)

    def isReadOnly(self):  # noqa: N802
        return self.__dict__.get("_read_only", False)


class QDateEdit(QDateTimeEdit):
    _FORMAT = "yyyy-MM-dd"

    def __init__(self, date=None, parent=None):
        super().__init__(date if date is not None else QDate(2000, 1, 1), parent)


class QTimeEdit(QDateTimeEdit):
    _FORMAT = "HH:mm"

    def __init__(self, time=None, parent=None):
        super().__init__(time if time is not None else QTime(0, 0), parent)


# ==========================================================================
# Calendar


class QCalendarWidget(QWidget):
    """A month of day buttons (the selected one pressed in, Blender's UI_SELECT) under a header row
    with the month's dropdown, the year's number field and arrows to the months before and after."""
    NoSelection, SingleSelection = 0, 1
    NoHorizontalHeader, SingleLetterDayNames, ShortDayNames, LongDayNames = 0, 1, 2, 3
    NoVerticalHeader, ISOWeekNumbers = 0, 1
    selectionChanged = Signal()
    clicked = Signal(object)
    activated = Signal(object)
    currentPageChanged = Signal(int, int)

    def __init__(self, parent=None):
        super().__init__(parent)
        self._selected = QDate.currentDate()
        self._min, self._max = QDate(100, 1, 1), QDate(9999, 12, 31)
        self._first = Qt.Monday
        self._page = (self._selected.year(), self._selected.month())
        self._mode = self.SingleSelection
        self._header_format = self.ShortDayNames
        self._grid_visible = False
        root = QVBoxLayout(self)
        self._nav = QWidget()
        nav = QHBoxLayout(self._nav)
        nav.setContentsMargins(0)
        self._prev, self._next = QPushButton(""), QPushButton("")
        self._prev.setIcon(QIcon("TRIA_LEFT"))
        self._next.setIcon(QIcon("TRIA_RIGHT"))
        self._month = QComboBox()
        self._month.addItems(_MONTHS)
        self._year = QSpinBox()
        self._year.setRange(100, 9999)
        for w in (self._prev, self._month, self._year, self._next):
            nav.addWidget(w)
        self._prev.clicked.connect(lambda *_: self.showPreviousMonth())
        self._next.clicked.connect(lambda *_: self.showNextMonth())
        self._month.activated.connect(lambda i: self.setCurrentPage(self._page[0], i + 1))
        self._year.valueChanged.connect(lambda y: self.setCurrentPage(y, self._page[1]))
        root.addWidget(self._nav)
        self._grid = QGridLayout()
        self._grid.setSpacing(0)
        root.addLayout(self._grid)
        self._heads = [QLabel("") for _ in range(7)]
        for c, lab in enumerate(self._heads):
            lab.setAlignment(Qt.AlignHCenter)
            self._grid.addWidget(lab, 0, c)
        self._days = []  # (button, its date) for the 6 x 7 cells
        for r in range(6):
            for c in range(7):
                b = QPushButton("")
                b.setMinimumWidth(round(1.2 * buipy.kit_unit()))  # seven fit a narrow calendar
                b.clicked.connect(lambda *_, k=len(self._days): self._pick(k))
                self._grid.addWidget(b, r + 1, c)
                self._days.append([b, QDate()])
        for c in range(7):
            self._grid.setColumnStretch(c, 1)
        self._fill()

    # -- the selection ---------------------------------------------------------------------------------------
    def selectedDate(self):  # noqa: N802
        return self._selected

    def setSelectedDate(self, d):  # noqa: N802
        """Within the range; shows its month; selectionChanged when it changes."""
        if not d.isValid() or d < self._min or d > self._max or self._mode == self.NoSelection:
            return
        changed = d != self._selected
        self._selected = d
        self.setCurrentPage(d.year(), d.month())
        self._fill()
        if changed:
            self.selectionChanged.emit()

    def selectionMode(self):  # noqa: N802
        return self._mode

    def setSelectionMode(self, mode):  # noqa: N802
        self._mode = int(mode)
        self._fill()

    def _pick(self, k):
        d = self._days[k][1]
        if d < self._min or d > self._max:
            return
        self.setSelectedDate(d)
        self.clicked.emit(d)
        self.activated.emit(d)

    # -- the range -----------------------------------------------------------------------------------------
    def minimumDate(self):  # noqa: N802
        return self._min

    def maximumDate(self):  # noqa: N802
        return self._max

    def setDateRange(self, lo, hi):  # noqa: N802
        self._min, self._max = lo, max(lo, hi)
        if self._selected < self._min or self._selected > self._max:
            self.setSelectedDate(max(self._min, min(self._max, self._selected)))
        self._fill()

    def setMinimumDate(self, d):  # noqa: N802
        self.setDateRange(d, max(d, self._max))

    def setMaximumDate(self, d):  # noqa: N802
        self.setDateRange(min(d, self._min), d)

    def clearMinimumDate(self):  # noqa: N802
        self.setMinimumDate(QDate(100, 1, 1))

    def clearMaximumDate(self):  # noqa: N802
        self.setMaximumDate(QDate(9999, 12, 31))

    # -- the page shown ----------------------------------------------------------------------------------------
    def monthShown(self):  # noqa: N802
        return self._page[1]

    def yearShown(self):  # noqa: N802
        return self._page[0]

    def setCurrentPage(self, year, month):  # noqa: N802
        if (int(year), int(month)) == self._page:
            return
        self._page = (int(year), int(month))
        self._fill()
        self.currentPageChanged.emit(*self._page)

    def _step_page(self, months):
        d = QDate(self._page[0], self._page[1], 1).addMonths(months)
        self.setCurrentPage(d.year(), d.month())

    def showNextMonth(self):  # noqa: N802
        self._step_page(1)

    def showPreviousMonth(self):  # noqa: N802
        self._step_page(-1)

    def showNextYear(self):  # noqa: N802
        self._step_page(12)

    def showPreviousYear(self):  # noqa: N802
        self._step_page(-12)

    def showToday(self):  # noqa: N802
        today = QDate.currentDate()
        self.setCurrentPage(today.year(), today.month())

    def showSelectedDate(self):  # noqa: N802
        self.setCurrentPage(self._selected.year(), self._selected.month())

    # -- look ----------------------------------------------------------------------------------------------
    def firstDayOfWeek(self):  # noqa: N802
        return self._first

    def setFirstDayOfWeek(self, day):  # noqa: N802
        self._first = int(day)
        self._fill()

    def horizontalHeaderFormat(self):  # noqa: N802
        return self._header_format

    def setHorizontalHeaderFormat(self, fmt):  # noqa: N802
        self._header_format = int(fmt)
        self._fill()

    def isNavigationBarVisible(self):  # noqa: N802
        return self._nav.isVisible()

    def setNavigationBarVisible(self, b):  # noqa: N802
        self._nav.setVisible(bool(b))

    def isGridVisible(self):  # noqa: N802
        return self._grid_visible

    def setGridVisible(self, b):  # noqa: N802
        """Grid lines: the day buttons drawn embossed (Blender's boxes) instead of flat."""
        self._grid_visible = bool(b)
        self._fill()

    def _fill(self):
        """The page: day names, then six weeks from the week the 1st is in."""
        y, m = self._page
        self._month.blockSignals(True)
        self._year.blockSignals(True)
        self._month.setCurrentIndex(m - 1)
        self._year.setValue(y)
        self._month.blockSignals(False)
        self._year.blockSignals(False)
        for c, lab in enumerate(self._heads):
            name = _DAYS[(self._first - 1 + c) % 7]
            lab.setText({self.NoHorizontalHeader: "", self.SingleLetterDayNames: name[:1],
                         self.ShortDayNames: name[:3]}.get(self._header_format, name))
        first = QDate(y, m, 1)
        start = first.addDays(-((first.dayOfWeek() - self._first) % 7))
        for k, cell in enumerate(self._days):
            d = start.addDays(k)
            cell[1] = d
            button = cell[0]
            button.setText(str(d.day()))
            button.setEnabled(self._min <= d <= self._max)
            selected = d == self._selected and self._mode != self.NoSelection
            # the selected day is an embossed, selected button (Blender's UI_SELECT); the rest flat
            button.setFlat(not (selected or self._grid_visible))
            button.set("select", selected)

