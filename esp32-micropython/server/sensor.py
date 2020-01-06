import os
import time
import json
import machine
from _thread import start_new_thread

from util import epoch_time

def average_excluding_outliers(values):
    values.sort()
    values = values[2:-2]
    return sum(values) / len(values)

def median(values):
    return values[len(values)//2]

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

    def multiply_then_average(self, other_buffer):
        values_pairs = list(zip(self._filled_buffer(), other_buffer._filled_buffer()))
        return sum(a * b for a, b in values_pairs) / len(values_pairs)


class Sensor:
    bits = 12
    max_voltage = 1
    max_value = 2 ** bits
    factor = max_voltage / max_value
    _average_x_reads = 5

    def __init__(self, voltage_pin, current_pin, voltage_factor, current_zero, current_factor,
            nominal_voltage=None, nominal_current=None, buffer_size=10):
        self.last_read = None
        self.cumulative_energy = 0

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
        this_read = time.ticks_ms()
        if self.voltage_adc:
            self.voltage_buffer.push(self.voltage_adc.read() * self.factor * self.voltage_factor)
        if self.current_adc:
            # val = self.current_adc.read()
            # print(val)
            # val = val * self.factor
            # print(val)
            # val = val - self.current_zero
            # print(val)
            # val = val * self.current_factor
            # print(val)
            # print("")


            reads = [None] * self._average_x_reads
            for i in range(len(reads)):
                # time.sleep_us(1)
                reads[i] = self.current_adc.read()
            value = average_excluding_outliers(reads)

            # value = self.current_adc.read()

            # print("%.3f: %s -> %s" % (((value * self.factor) - self.current_zero) * self.current_factor, sorted(reads), value))
            measured_current = ((value * self.factor) - self.current_zero) * self.current_factor
            previous_current = self.current_buffer.latest()
            if previous_current is not None:
                difference = measured_current - previous_current
                if abs(difference) > 0.1:
                    measured_current = previous_current + difference * 0.1
            self.current_buffer.push(measured_current)
        if self.last_read is not None:
            self.cumulative_energy += self.voltage_buffer.latest() * self.current_buffer.latest() * time.ticks_diff(this_read, self.last_read) / 1000
        self.last_read = this_read

    @property
    def voltage(self):
        return self.voltage_buffer.average()

    @property
    def current(self):
        return self.current_buffer.average()

    @property
    def power(self):
        return self.voltage_buffer.multiply_then_average(self.current_buffer)

    def pop_energy(self):
        value = self.cumulative_energy
        self.cumulative_energy = 0
        return value


class SensorLogger:
    _read_every_ms = 10
    _average_over_reads = 50
    _log_every_x_reads = 1000
    _rotate_period = 3600
    _max_files = 48
    _initial_meta = {
        "next_log": 0,
        "logs": {}
        }

    def __init__(self, log_directory, sensors_config):
        self.started = False
        self._stop_now = True

        # Setting up sensors:
        self.sensors = {}
        for name, config in sensors_config.items():
            self.sensors[name] = Sensor(**config, buffer_size=self._average_over_reads)

        self.last_read = time.ticks_ms()
        self.last_rotate = None

        # Setting up logging:
        self.reads_since_last_log = 0
        self.log_directory = log_directory.rstrip("/")
        self.start_time_offset = None
        self.our_logs_without_start_time = []
        self.log_index = None
        self.data_file = None
        try:
            os.mkdir(self.log_directory)
        except Exception:
            pass # Directory already exists

        self.load_meta()

    def rotate(self):
        # print("rotating from %s" % self.log_index)
        self.last_rotate = time.ticks_ms()
        if self.data_file:
            self.data_file.close()

        self.log_index = self.meta["next_log"]
        if self.log_index >= self._max_files:
            self.log_index = 0
        self.meta["next_log"] = self.log_index + 1

        self.data_file_path = "%s/%s.csv" % (self.log_directory, self.log_index)
        self.our_log_meta = {}
        if self.start_time_offset is None:
            self.our_logs_without_start_time.append(self.our_log_meta)
        self.meta["logs"][str(self.log_index)] = self.our_log_meta
        self.our_log_meta.update({
            "start_time": time.ticks_ms(),
            "start_time_offset": self.start_time_offset,
            })
        self.save_meta()

        # print("rotated to %s" % self.log_index)

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


    def refresh(self):
        # Seeing if it's time to do work yet:
        next_read = time.ticks_add(self.last_read, self._read_every_ms)
        time_till_next_work = time.ticks_diff(next_read, time.ticks_ms())
        if time_till_next_work <= 0:
            # Reading:
            self.read_all()
            # print("%s,%s" % (self.sensors["out"].current_buffer.latest(), self.sensors["in"].current_buffer.latest()))

            #Logging:
            self.reads_since_last_log += 1
            if self.reads_since_last_log >= self._log_every_x_reads:
                self.log_all()
                self.reads_since_last_log = 0

            # Log rotation:
            if self.last_rotate is None or (time.ticks_diff(time.ticks_ms(), self.last_rotate) / 1000) > self._rotate_period:
                self.rotate()
        return time_till_next_work

    def run_forever(self):
        self.started = True
        while not self._stop_now:
            sleep_ms = self.refresh()

            if sleep_ms > 0:
                # Sleep until next scheduled read:
                time.sleep_ms(sleep_ms)

    def read_all(self):
        for sensor_name, sensor in self.sensors.items():
            sensor.read()

    def get_voltage(self, sensor_name):
        return self.sensors[sensor_name].voltage

    def get_current(self, sensor_name):
        return self.sensors[sensor_name].current

    def get_power(self, sensor_name):
        sensor = self.sensors[sensor_name]
        return sensor.power

    def log_all(self):
        now = time.ticks_ms()
        # print("Logging at %s" % now)
        line = "%s\n" % ",".join([
            str(now),
            str(self.sensors["in"].pop_energy()),
            str(self.sensors["out"].pop_energy()),
            ])
        self.data_file.write(line)
        self.data_file.flush()
        # print(line, end="")

    def time_updated(self):
        """
        Should be called once the system time is set correctly (ex. from NTP
        or a browser client).
        Updates the metadata now that we know the real time of the data we've been
        collecting.
        """
        self.start_time_offset = epoch_time() * 1000 - time.ticks_ms()
        for log in self.our_logs_without_start_time:
            log["start_time_offset"] = self.start_time_offset
        self.our_logs_without_start_time = []
        self.save_meta()