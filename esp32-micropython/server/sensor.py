import os
import time
import json
import machine
from _thread import start_new_thread

from util import epoch_time

class CircularBuffer:
    def __init__(self, size):
        self._size = size
        self._buffer = [None] * size
        self._index = -1
        self._filled = False

    def push(self, value):
        self._index += 1
        if self._index >= self._size:
            self._index = 0
            self._filled = True
        self._buffer[self._index] = value

    def _filled_buffer(self):
        if self._filled:
            return self._buffer
        else:
            return self._buffer[:self._index+1]

    def average(self):
        values = self._filled_buffer()
        return sum(values) / len(values)

    def max(self):
        return max(self._filled_buffer())

    def min(self):
        return max(self._filled_buffer())

    def latest(self):
        return self._buffer[self._index]


class Sensor:
    bits = 12
    max_voltage = 1
    max_value = 2 ** bits
    factor = max_voltage / max_value

    def __init__(self, voltage_pin, current_pin, voltage_factor, current_zero, current_factor, nominal_voltage=None, nominal_current=None, buffer_size=10):
        self.voltage_buffer = CircularBuffer(buffer_size)
        if voltage_pin is not None:
            self.voltage_adc = machine.ADC(machine.Pin(voltage_pin))
        elif nominal_voltage is not None:
            self.voltage_adc = None
            self.voltage_buffer.push(nominal_voltage)
        else:
            raise ValueError("Must specify either voltage_pin or nominal_voltage")

        self.current_buffer = CircularBuffer(buffer_size)
        if current_pin is not None:
            self.current_adc = machine.ADC(machine.Pin(current_pin))
        elif nominal_current is not None:
            self.current_adc = None
            self.current_buffer.push(nominal_current)
        else:
            raise ValueError("Must specify either current_pin or nominal_current")

        self.voltage_factor = voltage_factor
        self.current_zero = current_zero
        self.current_factor = current_factor

    def read(self):
        if self.voltage_adc:
            self.voltage_buffer.push(self.voltage_adc.read() * self.factor * self.voltage_factor)
        if self.current_adc:
            self.current_buffer.push(((self.current_adc.read() * self.factor) - self.current_zero) * self.current_factor)

    def current_voltage(self):
        return self.voltage_buffer.latest()

    def current_current(self):
        return self.current_buffer.latest()

    def current_power(self):
        return self.current_voltage() * self.current_current()


class SensorLogger:
    _read_every_seconds = 0.2
    _log_every_x_reads = 5
    _initial_meta = {
        "next_log": 0,
        "logs": {}
        }

    def __init__(self, log_directory, sensors_config, max_files=60, rotate_period=60):
        self.started = False
        self._stop_now = True

        # Setting up sensors:
        self.sensors = {}
        for name, config in sensors_config.items():
            self.sensors[name] = Sensor(**config, buffer_size=self._log_every_x_reads)

        self.last_read = None
        self.last_rotate = None

        # Setting up logging:
        self.log_directory = log_directory.rstrip("/")
        self.start_time_offset = None
        self.our_logs_without_start_time = []
        self.log_index = None
        self.data_file = None
        self.max_files = max_files
        self.rotate_period = rotate_period
        try:
            os.mkdir(self.log_directory)
        except Exception:
            pass # Directory already exists

        self.load_meta()

    def rotate(self):
        print("rotating from %s" % self.log_index)
        self.last_rotate = time.ticks_ms()
        if self.data_file:
            self.data_file.close()

        self.log_index = self.meta["next_log"]
        if self.log_index >= self.max_files:
            self.log_index = 0
        self.meta["next_log"] = self.log_index + 1

        self.data_file_path = "%s/%s.csv" % (self.log_directory, self.log_index)
        self.our_log_meta = {}
        if self.start_time_offset is None:
            self.our_logs_without_start_time.append(self.our_log_meta)
        self.meta["logs"][str(self.log_index)] = self.our_log_meta
        self.our_log_meta.update({
            "start_time": int(time.ticks_ms() / 1000),
            "start_time_offset": self.start_time_offset,
            })
        self.save_meta()

        print("rotated to %s" % self.log_index)

        self.data_file = open(self.data_file_path, "w")

    def load_meta(self):
        try:
            with open("%s/%s" % (self.log_directory, "meta.json"), "r") as f:
                try:
                    self.meta = json.loads(f.read())
                except ValueError:
                    self.meta = self._initial_meta
        except OSError:
            self.meta = self._initial_meta

        try:
            self.meta["next_log"]
            self.meta["logs"].get
        except (KeyError, AttributeError):
            self.meta = self._initial_meta

    def save_meta(self):
        with open("%s/%s" % (self.log_directory, "meta.json"), "w") as f:
            f.write(json.dumps(self.meta))

    def start(self, threaded=False):
        if not self.started:
            self._stop_now = False
            if threaded:
                start_new_thread(self.run_forever, ())
            else :
                self.run_forever()

    def stop(self):
        if self.started:
            self._stop_now = True

    def run_forever(self):
        self.started = True
        self.last_read = time.ticks_ms()
        reads_since_last_log = 0
        while not self._stop_now:
            if self.last_rotate is None or (time.ticks_diff(time.ticks_ms(), self.last_rotate) / 1000) > self.rotate_period:
                self.rotate()
            next_read = time.ticks_add(self.last_read, int(self._read_every_seconds * 1000))
            sleep_ms = time.ticks_diff(next_read, time.ticks_ms())
            if sleep_ms > 0:
                time.sleep(sleep_ms / 1000)
            self.last_read = time.ticks_ms()
            self.read_all()
            reads_since_last_log += 1
            if reads_since_last_log >= self._log_every_x_reads:
                self.log_all()
                reads_since_last_log = 0

        self.started = False

    def read_all(self):
        for sensor_name, sensor in self.sensors.items():
            sensor.read()

    def get_voltage(self, sensor_name):
        return self.sensors[sensor_name].voltage_buffer.latest()

    def get_current(self, sensor_name):
        return self.sensors[sensor_name].current_buffer.latest()

    def get_power(self, sensor_name):
        sensor = self.sensors[sensor_name]
        return sensor.voltage_buffer.latest() * sensor.current_buffer.latest()

    def log_all(self):
        now = int(time.ticks_ms() / 1000)
        print("Logging at %s" % now)
        self.data_file.write("%s\n" % ",".join([
            str(now),
            str(self.sensors["in"].voltage_buffer.average()),
            str(self.sensors["in"].current_buffer.average()),
            str(self.sensors["out"].voltage_buffer.average()),
            str(self.sensors["out"].current_buffer.average()),
            ]))
        self.data_file.flush()


    def time_updated(self):
        """
        Should be called once the system time is set correctly (ex. from NTP
        or a browser client).
        Updates the metadata now that we know the real time of the data we've been
        collecting.
        """
        self.start_time_offset = epoch_time() - int(time.ticks_ms() / 1000)
        for log in self.our_logs_without_start_time:
            log["start_time_offset"] = self.start_time_offset
        self.our_logs_without_start_time = []
        self.save_meta()