import os
import time
import json
import machine
from _thread import start_new_thread
from ads1x15 import ADS1115

from util import epoch_time
from logger import log_exception

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

    def multiply_then_max(self, other_buffer):
        values_pairs = list(zip(self._filled_buffer(), other_buffer._filled_buffer()))
        return max(a * b for a, b in values_pairs)


class BaseADC:
    def __init__(self, settings={}, **kwargs):
        raise NotImplementedError()

    def read(self):
        raise NotImplementedError()

class BuiltInADC(BaseADC):
    bits = 12
    max_voltage = 1 # The ESP ADC reads values between 0-1V despite operating at 3.3V.
    max_value = 2 ** bits
    factor = max_voltage / max_value
    def __init__(self, pin, settings={}):
        self.adc = machine.ADC(machine.Pin(pin))

    def read(self):
        return self.adc.read() * self.factor

i2c = None

class ADS1115ADC(BaseADC):
    def __init__(self, address, channels, settings={}):
        global i2c
        if i2c is None:
            print("Initializing I2C")
            i2c_settings = settings["i2c"]
            i2c = machine.I2C(
                settings["i2c"].get("id", -1),
                scl=machine.Pin(i2c_settings["scl"]),
                sda=machine.Pin(i2c_settings["sda"]),
                freq=i2c_settings.get("freq", None),
                )

        # Voltage is max readable voltage (full-scale input voltage range: FSR).
        # Doesn't let you read above power supply voltage (VCC) of 3.3V or 5V.
        # So for maximum resolution, pick 1x for 5V and 2x for 3.3V and make sure
        # input voltages are less than 4.096V and 2.048V respectively

        # gain_index = 0 # 2/3x 6.144V
        # gain_index = 1 # 1x   4.096V
        gain_index = 2 # 2x   2.048V
        # gain_index = 3 # 4x   1.024V
        # gain_index = 4 # 8x   0.512V
        # gain_index = 5 # 16x  0.256V

        self.adc = ADS1115(i2c, address, gain_index)
        self.channels = channels

        # rate_index is an index into how many samples can be taken per second.
        # More samples per second means faster reads but noisier results.
        # self.rate_index = 0 # 8    samples per second
        # self.rate_index = 1 # 16   samples per second
        # self.rate_index = 2 # 32   samples per second
        # self.rate_index = 3 # 64   samples per second
        # self.rate_index = 4 # 128  samples per second
        # self.rate_index = 5 # 250  samples per second
        # self.rate_index = 6 # 475  samples per second
        self.rate_index = 7 # 860  samples per Second

    def read(self):
        try:
            raw_value = self.adc.read(self.rate_index, *self.channels)
            voltage = self.adc.raw_to_v(raw_value)
            # print("%s,%s" % (raw_value, voltage))
            return voltage
        except OSError:
            return 0 # TODO: update callers to handle None

class Sensor:
    # _average_x_reads = 5

    def __init__(self, voltage_adc, current_adc, voltage_factor, current_zero, current_factor,
            nominal_voltage=None, nominal_current=None, buffer_size=10, settings={}):
        self.last_read = None
        self.cumulative_energy = 0
        self.available_cumulative_energy = 0

        sensor_type = settings["sensor_type"]
        types = {
            "ADS1115": ADS1115ADC,
            "BuiltInADC": BuiltInADC,
        }
        ADC = types.get(sensor_type, None)
        if ADC is None:
            raise Exception("Unrecognized sensor type: %s. Pick one of: %s." % (sensor_type, ", ".join(types.keys())))

        self.voltage_buffer = CircularBuffer(buffer_size)
        if voltage_adc is not None:
            self.voltage_adc = ADC(settings=settings, **voltage_adc)
        elif nominal_voltage is not None:
            self.voltage_adc = None
            self.voltage_buffer.push(nominal_voltage)
        else:
            raise ValueError("Must specify either voltage_adc or nominal_voltage")

        self.current_buffer = CircularBuffer(buffer_size)
        if current_adc is not None:
            self.current_adc = ADC(settings=settings, **current_adc)
        elif nominal_current is not None:
            self.current_adc = None
            self.current_buffer.push(nominal_current)
        else:
            raise ValueError("Must specify either current_adc or nominal_current")

        self.voltage_factor = voltage_factor
        self.current_zero = current_zero
        self.current_factor = current_factor

    def read(self):
        this_read = time.ticks_ms()
        if self.voltage_adc:
            self.voltage_buffer.push(self.voltage_adc.read() * self.voltage_factor)
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


            # reads = [None] * self._average_x_reads
            # for i in range(len(reads)):
            #     # time.sleep_us(1)
            #     reads[i] = self.current_adc.read()
            # value = average_excluding_outliers(reads)

            value = self.current_adc.read()

            # print("%.3f: %s -> %s" % (((value * self.factor) - self.current_zero) * self.current_factor, sorted(reads), value))
            measured_current = (value - self.current_zero) * self.current_factor
            # previous_current = self.current_buffer.latest()
            # if previous_current is not None:
            #     difference = measured_current - previous_current
            #     if abs(difference) > 0.1:
            #         measured_current = previous_current + difference * 0.1
            self.current_buffer.push(measured_current)
        if self.last_read is not None:
            power = self.voltage_buffer.latest() * self.current_buffer.latest()
            duration = time.ticks_diff(this_read, self.last_read) / 1000
            available_power = self.voltage_buffer.multiply_then_max(self.current_buffer)
            # print("power: %.1f, available: %.1f" % (power, available_power))
            available_power = max(power, available_power * 0.9)  # Assuming only 90% of the available
                                                                 # can be used. To allow for outlier readings,
                                                                 # < 100% efficiency, etc...
            self.cumulative_energy += power * duration
            self.available_cumulative_energy += available_power * duration
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

    @property
    def available_power(self):
        available_power = self.voltage_buffer.multiply_then_max(self.current_buffer)
        return max(self.power, available_power * 0.9) # Assuming only 90% of the available
                                                 # can be used. To allow for outlier readings,
                                                 # < 100% efficiency, etc...

    @property
    def duty(self):
        return self.power / self.available_power

    def pop_energy(self):
        value = self.cumulative_energy
        self.cumulative_energy = 0
        return value

    def pop_available_energy(self):
        value = self.available_cumulative_energy
        self.available_cumulative_energy = 0
        return value


class SensorLogger:
    _read_every_ms = 10
    _average_over_reads = 20
    _log_every_x_reads = 1000
    _rotate_period = 3600
    _max_files = 170

    def __init__(self, log_directory, sensor_config, settings={}):
        self.started = False
        self._stop_now = True

        # Setting up sensors:
        self.sensors = {}
        for name, config in sensor_config.items():
            self.sensors[name] = Sensor(**config, buffer_size=self._average_over_reads, settings=settings)

        self.last_read = time.ticks_ms()
        self.last_rotate = None

        # Setting up logging:
        self.reads_since_last_log = 0
        self.log_directory = log_directory.rstrip("/")
        self.meta_file = "%s/%s" % (self.log_directory, "meta.csv")
        self.tmp_meta = "%s/%s" % (self.log_directory, "tmp_meta.csv")
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

        if self.log_index >= self._max_files:
            self.log_index = 0
        else:
            self.log_index += 1

        self.data_file_path = "%s/%s.csv" % (self.log_directory, self.log_index)

        with open(self.meta_file, "r") as f:
            with open(self.tmp_meta, "w") as f2:
                # print("rotate():")
                str_log_index = str(self.log_index)
                updated_line = False
                for line in f.readlines():
                    # print("<%s>" % line.strip())
                    index, active, start_time, start_time_offset = line.strip().split(",")
                    if index == str_log_index:
                        updated_line = True
                        active = "1"
                        start_time = str(time.ticks_ms())
                        start_time_offset = str(self.start_time_offset)
                    else:
                        active = "0"
                    line2 = ",".join([index, active, start_time, start_time_offset])
                    # print("[%s]" % line2)
                    f2.write(line2)
                    f2.write("\n")
                if updated_line is False:
                    line2 = ",".join([str_log_index, "1", str(time.ticks_ms()), str(self.start_time_offset)])
                    # print("[%s]" % line2)
                    f2.write(line2)
                    f2.write("\n")

        os.rename(self.tmp_meta, self.meta_file)
        # print("rotated to %s" % self.log_index)

        self.data_file = open(self.data_file_path, "w")

    def load_meta(self):
        try:
            with open(self.meta_file, "r") as f:
                with open(self.tmp_meta, "w") as f2:
                    # print("load_meta():")
                    # Looping over all entries in meta.csv.
                    # For each, setting start_time_offset from None to Unkn.
                    # Also, extracting the current index.
                    for line in f.readlines():
                        # print("<%s>" % line.strip())
                        index, active, start_time, start_time_offset = line.strip().split(",")
                        if active == "1":
                            self.log_index = int(index)
                            active = "0"
                        if start_time_offset == "None":
                            start_time_offset = "Unknown"
                        line2 = ",".join([index, active, start_time, start_time_offset])
                        # print("[%s]" % line2)
                        f2.write(line2)
                        f2.write("\n")

            os.rename(self.tmp_meta, self.meta_file)
        except OSError as e:
            err = "Failed to load meta.csv: %s\n" % e
            print(err)
            log_exception(err)
            # Initializing an empty file:
            with open(self.meta_file, "w") as f:
                pass

        if self.log_index is None:
            self.log_index = self._max_files

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
        # print("time_till_next_work: %s" % time_till_next_work)
        if time_till_next_work <= 0:

            # Log rotation:
            if self.last_rotate is None or (time.ticks_diff(time.ticks_ms(), self.last_rotate) / 1000) > self._rotate_period:
                self.rotate()

            # Reading:
            self.read_all()
            # print("%.4f,%.4f,%.4f,%.4f" % (
            #     self.sensors["in"].voltage_buffer.latest(),
            #     self.sensors["out"].voltage_buffer.latest(),
            #     self.sensors["in"].current_buffer.latest(),
            #     self.sensors["out"].current_buffer.latest(),
            #     ))

            #Logging:
            self.reads_since_last_log += 1
            if self.reads_since_last_log >= self._log_every_x_reads:
                self.log_all()
                self.reads_since_last_log = 0

        return time_till_next_work

    def run_forever(self):
        self.started = True
        while not self._stop_now:
            sleep_ms = self.refresh()

            if sleep_ms > 0:
                # Sleep until next scheduled read:
                time.sleep_ms(sleep_ms)

    def read_all(self):
        self.last_read = time.ticks_ms()
        for sensor_name, sensor in self.sensors.items():
            sensor.read()
        # print("Read in %.3f ms" % time.ticks_diff(time.ticks_ms(), self.last_read))

    def get_voltage(self, sensor_name):
        return self.sensors[sensor_name].voltage

    def get_current(self, sensor_name):
        return self.sensors[sensor_name].current

    def get_power(self, sensor_name):
        return self.sensors[sensor_name].power

    def get_available_power(self, sensor_name):
        return self.sensors[sensor_name].available_power

    def get_duty(self, sensor_name):
        return self.sensors[sensor_name].duty

    def log_all(self):
        now = time.ticks_ms()
        # print("Logging at %s" % now)
        line = "%s,%.1f,%.1f,%.1f" % (
            now,
            self.sensors["in"].pop_energy(),
            self.sensors["out"].pop_energy(),
            self.sensors["in"].pop_available_energy(),
            )
        self.data_file.write(line)
        self.data_file.write("\n")
        self.data_file.flush()
        # print(line)

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

        with open(self.meta_file, "r") as f:
            with open(self.tmp_meta, "w") as f2:
                # print("time_updated():")
                for line in f.readlines():
                    # print("<%s>" % line.strip())
                    index, active, start_time, start_time_offset = line.strip().split(",")
                    if start_time_offset == "None":
                        start_time_offset = str(self.start_time_offset)


                    line2 = ",".join([index, active, start_time, start_time_offset])
                    # print("[%s]" % line2)
                    f2.write(line2)
                    f2.write("\n")

        os.rename(self.tmp_meta, self.meta_file)