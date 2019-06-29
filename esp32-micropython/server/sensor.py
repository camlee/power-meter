import time
import machine
from _thread import start_new_thread

class Sensor:
    bits = 12
    max_voltage = 1
    max_value = 2 ** bits
    factor = max_voltage / max_value

    def __init__(self, voltage_pin, current_pin, voltage_factor, current_zero, current_factor, nominal_voltage=None, nominal_current=None):
        if voltage_pin is not None:
            self.voltage_adc = machine.ADC(machine.Pin(voltage_pin))
            self.voltage = 0
        elif nominal_voltage is not None:
            self.voltage_adc = None
            self.voltage = nominal_voltage
        else:
            raise ValueError("Must specify either voltage_pin or nominal_voltage")

        if current_pin is not None:
            self.current_adc = machine.ADC(machine.Pin(current_pin))
            self.current = 0
        elif nominal_current is not None:
            self.current_adc = None
            self.current = nominal_current
        else:
            raise ValueError("Must specify either current_pin or nominal_current")

        self.voltage_factor = voltage_factor
        self.current_zero = current_zero
        self.current_factor = current_factor

        self.power = self.current * self.voltage

    def read(self):
        if self.voltage_adc:
            self.voltage = self.voltage_adc.read() * self.factor * self.voltage_factor
        if self.current_adc:
            self.current = ((self.current_adc.read() * self.factor) - self.current_zero) * self.current_factor
        self.power = self.voltage * self.current


class SensorLogger:
    _read_period = 0.1
    _log_period = 1
    _log_period_ms = _log_period * 1000

    def __init__(self, log_directory, sensors_config):
        self.started = False
        self._stop_now = True

        # Setting up sensors:
        self.sensors = {}
        for name, config in sensors_config.items():
            self.sensors[name] = Sensor(**config)

        # Setting up logging:
        self.log_directory = log_directory
        self.last_log = None

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
        while not self._stop_now:
            time.sleep(self._read_period)
            self.read_all()
            self.log_all()
        self.started = False

    def read_all(self):
        for sensor_name, sensor in self.sensors.items():
            sensor.read()

    def get_voltage(self, sensor_name):
        return self.sensors[sensor_name].voltage

    def get_current(self, sensor_name):
        return self.sensors[sensor_name].current

    def get_power(self, sensor_name):
        return self.sensors[sensor_name].power

    def log_all():
        if self.last_log is None or time.ticks_ms() > (self.last_log + self._log_period_ms):
            print("Logging at %.1f" % time.ticks_ms() / 1000)