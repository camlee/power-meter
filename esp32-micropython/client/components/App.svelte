<script>
  import "@material/layout-grid/mdc-layout-grid";

  import PowerUsage from './PowerUsage.svelte';
  import DiskUsage from './DiskUsage.svelte';
  import Realtime from './Realtime.svelte';
  import FullPageProgress from './FullPageProgress.svelte';

  import {processLogResponse, processDataMeta, readLogResponseWithProgress} from '../common/DataProcessing.js';

  let API_URL = process.env.API_URL || "http://" + location.host;
  let WS_URL = process.env.WS_URL || "ws://" + location.host;

  function getTimeParameter(){
    let time = ((new Date()).getTime() / 1000).toFixed(); // Unix epoch time in seconds
    return "time=" + time;
  }

  let loading = true;
  let loading_verb = "Connecting";
  let loading_action = "Connect";
  let loading_error = "";

  let getting_stats_handler;
  let getting_stats = new Promise(function(resolve, reject){
    getting_stats_handler = {resolve: resolve, reject: reject};
  });

  async function get_stats(){
    try {
      var response = await fetch(API_URL + '/stats');
    } catch(e){
      getting_stats_handler.reject(e);
      return;
    }
    if (!response.ok){
      getting_stats_handler.reject(response.statusText);
      return;
    }
    try {
      var stats = await response.json();
    } catch(e){
      getting_stats_handler.reject(e);
    }
    getting_stats_handler.resolve(stats);
  }

  let historical_data = []
  let getting_data_progress = false;
    // false for not getting data yet.
    // null for getting data but don't know the progress
    // Number between 0 and 1 to indicate progress.
  let getting_data_error = null;

  let fetched_logs = [];

  async function* getData(){
    getting_data_progress = null;
    let response = await fetch(API_URL + '/data/meta.csv');
    if (!response.ok){
      throw new Error(response.statusText);
    }
    const meta = await response.text();
    let logs = processDataMeta(meta);
    let original_log_file_count = logs.length;

    // Skip data that doesn't have complete time information:
    logs = logs.filter(function(a){
      return !(a.start_time_offset === null);
    });

    let valid_log_file_count = logs.length;

    // Skip data that we already have:
    logs = logs.filter(function(a){
      return !fetched_logs.some(function(b){
        return b.active == false && a.number == b.number && a.start_time_offset == b.start_time_offset && a.start_time == b.start_time;
      })
    });
    console.log(`Number of log files: total: ${original_log_file_count} valid: ${valid_log_file_count} new: ${logs.length}`);

    // Load the newest data first as that's most likely to be viewed first:
    logs.sort(function(a, b) {
      let a_value = a.start_time_offset + a.start_time;
      let b_value = b.start_time_offset + b.start_time;
      return b_value - a_value;
    })

    getting_data_progress = 0;

    // Download the data:
    for (const [log_index, log] of logs.entries()){
      console.log(`Downloading ${log.number}.csv (${new Date(log.start_time_offset + log.start_time)})`);

      let response = await fetch(API_URL + '/data/' + log.number + '.csv');

      const response_text = await readLogResponseWithProgress(response,
         function(downloading_progress) {
          let new_progress = (log_index / logs.length) + (downloading_progress / logs.length);
          if (new_progress > getting_data_progress){
            getting_data_progress = Math.round(new_progress * 100) / 100;
              // Rounding seems to fix a really weird problem where ther progress bar jumps
              // around slightly when doing chart updates. It also smoothes things out a
              // bit and makes it look better overall.
          }
        });

      // Seeing if we have downloaded part of this log file before and getting the time where we left off:
      let previous_logs = fetched_logs.filter(function(a){
        return a.active && a.number == log.number && a.start_time == log.start_time && a.start_time_offset == log.start_time_offset;
      });
      let last_processed_time = null;
      if (previous_logs.length > 0){
        let previous_log = previous_logs[previous_logs.length-1];
        last_processed_time = previous_log.last_processed_time;
      }

      // const response_text = await response.text();
      let data = await processLogResponse(response_text, log.start_time_offset, last_processed_time);
      if (data.length > 0){
        log.last_processed_time = data[data.length-1][0];
        fetched_logs.push(log);
      }
      console.log(`New Data points from ${log.number} since ${last_processed_time}: ${data.length}`);
      yield data;
    }
  }

  let historical_fetcher = null;
  let historical_fetcher_setup = false;
  let pull_historical_data_period_seconds = 300;

  function setup_historical_fetcher(){
    if (historical_fetcher_setup === false){
      historical_fetcher_setup = true;
      document.addEventListener("visibilitychange", function (){
        if (historical_fetcher != null){
          clearInterval(historical_fetcher);
          historical_fetcher = null;
        }
        if (!document.hidden){
          setup_historical_fetcher();
        }
      }, false);
    }

    get_historical_data();
    historical_fetcher = setInterval(function() {
      get_historical_data();
    }, pull_historical_data_period_seconds * 1000);
  }

  let getting_data = false;

  async function get_historical_data(){
    if (getting_data){
      console.log("Still getting data from last time.");
      return;
    }
    getting_data = true;
    try{
      for await (const data of getData()) {
        historical_data = [...historical_data, ...data];
      }
      getting_data_error = null;
    } catch(e){
      getting_data_error = e.message;
      console.log(`Error getting data: ${getting_data_error}`);
    }
    getting_data = false;
  }

  let websocket = null;
  let websocket_retry_period = 1;
  let close_websocket_after_tab_changed_for_seconds = 50;
  let websocket_visibilitychange_setup = false;
  let websocket_last_visibility_change_timeout = null;

  function setup_realtime_websocket(){
    if (websocket_visibilitychange_setup === false) {
      websocket_visibilitychange_setup = true;
      document.addEventListener("visibilitychange", function (){
        if (websocket_last_visibility_change_timeout != null){
          clearTimeout(websocket_last_visibility_change_timeout);
          websocket_last_visibility_change_timeout = null;
        }
        if (document.hidden){
          websocket_last_visibility_change_timeout = setTimeout(function() {
            if (websocket !== null){
              websocket.close();
            }
          }, close_websocket_after_tab_changed_for_seconds * 1000);

        } else {
          if (websocket === null){
            setup_realtime_websocket();
          }
        }
      }, false);
    }

    if (document.hidden){
      console.log("Not setting up websocket: page not visible.");
      return;
    }

    console.log("Setting up websocket.");
    websocket = new WebSocket(WS_URL);

    websocket.addEventListener("open", function(event){
      console.log("Websocket connected!");
      websocket_retry_period = 1;
      websocket.send(getTimeParameter());
    });

    websocket.addEventListener("error", function(error){
      console.log(`Websocket error (${error.message}). closing.`);
      websocket.close();
    });

    websocket.addEventListener("close", function(event){
      websocket = null;
      if (websocket_retry_period < 10){
        websocket_retry_period += 1;
      }
      if (document.visible){
        console.log(`Websocket closed (code: ${event.code}, reason: ${event.reason}). Reconnecting in ${websocket_retry_period} seconds.`)
        setTimeout(setup_realtime_websocket, websocket_retry_period * 1000);
      } else {
        console.log(`Page not visible: websocket closed (code: ${event.code}, reason: ${event.reason}).`);
      }
    });
  }


  async function main(){
    try {
      var response = await fetch(API_URL + "/set_time?" + getTimeParameter(), {"method": "POST"});
    } catch(e){
      loading_error = "Network Error.";
      return;
    }
    if (!response.ok){
      loading_error = response.statusText;
      return;
    }

    loading_verb = "Loading";
    loading_action = "Load";
    try {
      await import('chart.js');
    } catch(e){
      loading_error = e;
      return;
    }
    loading = false;
    get_stats();
    setup_historical_fetcher();
    setup_realtime_websocket();

  }

  main();
</script>

<main>

{#if loading}
  <FullPageProgress action={loading_action} action_verb={loading_verb} error={loading_error}/>
{:else}
  <h1>Power Meter</h1>

  <div class="mdc-layout-grid">
    <div class="mdc-layout-grid__inner">
      <div class="mdc-layout-grid__cell--span-12">
        <PowerUsage progress={getting_data_progress} data={historical_data} error={getting_data_error} refresh_data={get_historical_data}/>
      </div>
    </div>
  </div>

  <div class="mdc-layout-grid">
    <div class="mdc-layout-grid__inner">
      <div class="mdc-layout-grid__cell--span-12">
        <Realtime websocket={websocket} title="Realtime" y_label="Power [W]"
          series={[
            {
              index: 1,
              label: "Actual In",
              color: "blue",
            },
            {
              index: 2,
              label: "Total Usage",
              color: "orange",
            },
            {
              index: 7,
              label: "Panel Producing",
              color: "deepskyblue",
            },
            ]}/>
      </div>
    </div>
  </div>

  <div class="mdc-layout-grid">
    <div class="mdc-layout-grid__inner">
      <div class="mdc-layout-grid__cell--span-6">
        <Realtime websocket={websocket} title="Raw Current" y_label="Current [A]"
          series={[
            {
              index: 5,
              label: "In",
              color: "blue",
            },
            {
              index: 6,
              label: "Out",
              color: "orange",
            },
          ]}/>
      </div>
      <div class="mdc-layout-grid__cell--span-6">
        <Realtime websocket={websocket} title="Raw Voltage" y_label="Voltage [V]"
          series={[
            {
              index: 3,
              label: "In",
              color: "blue",
            },
            {
              index: 4,
              label: "Out",
              color: "orange",
            },
          ]}/>
      </div>
    </div>
  </div>
  <div class="mdc-layout-grid">
    <div class="mdc-layout-grid__inner">
      <div class="mdc-layout-grid__cell--span-6">
        <DiskUsage getting_stats={getting_stats}/>
      </div>
    </div>
  </div>
{/if}

</main>
