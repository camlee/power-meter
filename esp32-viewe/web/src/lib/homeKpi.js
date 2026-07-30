const WINDOW = 4;
const REGULAR_INTERVAL_MS = 2000;
const MINIMUM_STEP_WATTS = 5;
const RELATIVE_STEP = 0.05;

export class AdaptiveHomeKpi {
  constructor() {
    this.reset();
  }

  reset() {
    this.samples = [];
    this.initialized = false;
    this.displayedAvailable = false;
    this.displayed = 0;
    this.lastPublishMs = 0;
    this.significantCount = 0;
  }

  add(nowMs, sample) {
    this.samples = [...this.samples, sample].slice(-WINDOW);
    if (!this.initialized) {
      this.initialized = true;
      this.lastPublishMs = nowMs;
      this.displayedAvailable = Number.isFinite(sample);
      this.displayed = this.displayedAvailable ? sample : 0;
      return {
        publish: this.displayedAvailable,
        available: this.displayedAvailable,
        value: this.displayed,
      };
    }

    const significant = Number.isFinite(sample)
      ? (!this.displayedAvailable ||
        Math.abs(sample - this.displayed) >= Math.max(
          MINIMUM_STEP_WATTS, Math.abs(this.displayed) * RELATIVE_STEP,
        ))
      : this.displayedAvailable;
    this.significantCount = significant ? this.significantCount + 1 : 0;

    if (this.significantCount >= 2) {
      this.significantCount = 0;
      return this.publish(nowMs, 2);
    }
    if ((nowMs - this.lastPublishMs) >>> 0 >= REGULAR_INTERVAL_MS) {
      this.significantCount = 0;
      return this.publish(nowMs, WINDOW);
    }
    return { publish: false, available: false, value: 0 };
  }

  publish(nowMs, requested) {
    const finite = this.samples.slice(-requested).filter(Number.isFinite);
    this.lastPublishMs = nowMs;
    this.displayedAvailable = finite.length > 0;
    this.displayed = this.displayedAvailable
      ? finite.reduce((total, value) => total + value, 0) / finite.length
      : 0;
    return {
      publish: true,
      available: this.displayedAvailable,
      value: this.displayed,
    };
  }
}
