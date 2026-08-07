This document describe a new major feature to add to the power meter: connecting the meter to the internet for remote access of data. This is called telemetry and this document describes the goals and scope for building out the first version of this.

## Use Cases
* Monitor system health and correct functionality: ensure OTA upgrades take, sensors are reading sane values, etc...
* Monitor long-term trends in solar production, battery use, and load consumption.
* Debug issues and fix problems remotely: adjust calibration/configuration, etc...
* Gather data for future feature improvement: debugging sensor traces, generate battery capacity models, etc...
* Usage analytics: screen interactions, most-viewed pages, etc...

## Data classification
Roughly speaking, we can break things down into a few notably different types of data:
1. Realtime sensor data - web UI but remote.
2. Historical usage data - aggregated time series data.
3. Raw snapshots - detailed sensor readings for a short burst period.
4. Metadata - version, uptime, configuration, callibration, etc...
5. Analytics - web vs screen interactions, etc...

## Architecture Considerations
The device has sporadic outbound internet access. There's currently no server to device communication channel. To share data, we need two computers online and connected at once: the device and a server of some sort. We could potentially connect directly from an admin's laptop/phone to the device and try to provide a realtime web UI, but that would require a VPN of some sort (ex. tailscale), a reliable enough internet connection (latency and throughput would matter a lot for the UX), and critically - coordinated times to do things. These are all problematic for this use-case. A always-on server that the device checks-in to and then stores data for later retrieval by the admin is a much better fit for most of the use cases. It does not provide realtime sensor data, but would be great for historical usage, metadata, analytics, and raw snapshots could be made to work. If needed, a remote management feature could be built allowing the admin to queue-up certain actions to be performed on the device the next time it's online. More like e-mail than a phone call.

Many many options exist for server infrastructure, ranging from a self-hosted device (ex. just a raspberry pi on a shelf) to a full-blown AWS EC2 instance. All that's needed from the server are:
1) A simple endpoint (ex. HTTP) to accept incoming data from the device.
2) A database or storage of some sort to record the data.
3) A web app for admin access/retrieval.

To keep costs low, a really attractive option is serverless cloud functions and databases. These are modern cloud offerings where you upload your code and when a request/query comes in, it spins up within a fraction of a second to run and respond. When there's no activity going on, there's no server idling to pay for. Furthermore, there's options here with very generous free tiers, such as:

| Name | Free Limits | Pros | Cons |
|---|---|---|---|
| Cloudflare (Workers + D1) | 100k requests/day, 5 GB DB | No sleep/pause | 3 services to wire together |
| Turso | 5 GB storage, 500M reads/mo | Most portable | Needs separate compute |
| Supabase | 500MB DB, 1 GB storage | Instant REST API | Auto-pauses after 7 days |
| Netlify | 300 credits/mo |  | Volatile pricing |
| Firebase | 1 GB DB, 50k reads/day |  | Requires a credit card in case free limits exceeded |

## Scope
Given the above, the proposed scope for a first useable version is:
* Server architecture using serverless cloud provider.
* Device checks-in periodically (ex. daily) sends basic metadata: version, uptime, etc...
* Device flushes aggregated historical usage data: hourly data points for example.
* New custom admin interface hosted on the same cloud provider and provides a web UI to monitor and visualize.
* Admin interface must require login for security.
* Device telemetry should also require authentication of some sort: don't want garbage data from malicious actors.
* Backend should support receiving data from multiple devices and allow for viewing them seperately (and together???) in the admin interface. 

Potential scope that could be built on top of this base:
* Collecting sensor snapshots periodically or on-demand and storing those in the backend for viewing.
* Sending configuration data: callibration, sensor source, etc...
* WebSocket or other persistent connection to the backend - might allow for realtime sensor data and interactions.

## Design
Here's a more detailed proposed design, sketching out the full infrastructure and tech stack:
* Cloudflare - chosen because it is an industry leader, has all the required features, and provides a generous free tier.
* Typescript Worker endpoint - stay in the Javascript ecosystem (same as the existing web app)
* Drizzle ORM - Object Relational Mapper simplifies SQL Database stuff: table creation, querying, etc...
* New, custom admin UI app - many parts may be similar to the ESP32-hosted web app but most functionality is going to be somewhat different: login, database querying, etc... We may be able to design the web app for reuse, or extract reusable components but with AI coding, having two separate implementations we tell it to keep in-sync is not that big a deal. Also we already have that kind of duplication with our web app vs on-device (LVGL) display.
* Cloudflare Zero Trust for auth.
