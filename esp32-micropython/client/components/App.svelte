<script>
  import "@material/layout-grid/mdc-layout-grid";

  import PowerUsage from './PowerUsage.svelte';
  import DiskUsage from './DiskUsage.svelte';
  import FullPageProgress from './FullPageProgress.svelte';

  import {processLogResponse, processDataMeta} from '../common/DataProcessing.js';

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

  async function* getData(){
    getting_data_progress = null;
    let response = await fetch(API_URL + '/data/meta.json');
    const meta = await response.json();
    let logs = processDataMeta(meta);

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
      const response_text = await response.text();
      getting_data_progress = (log_index + 1) / logs.length;
      yield await processLogResponse(response_text, log.start_time_offset);
    }
  }

  async function get_historical_data(){
    try{
      for await (const data of getData()) {
        historical_data = [...historical_data, ...data];
      }
    } catch(e){
      getting_data_error = e.message;
      console.log(`Error getting data: ${getting_data_error}`);
    }
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
    get_historical_data();
  }

  main();
</script>

<style>
  .progress-bar {
    min-width: 300px;
  }
</style>

<main>

{#if loading}
  <FullPageProgress action={loading_action} action_verb={loading_verb} error={loading_error}/>
{:else}
  <h1>Power Meter</h1>
    <div class="mdc-layout-grid">
      <div class="mdc-layout-grid__inner">
        <div class="mdc-layout-grid__cell--span-6">
            <h2>Realtime</h2>
            <canvas id="realtime"/>
        </div>
        <div class="mdc-layout-grid__cell--span-6">
            <PowerUsage progress={getting_data_progress} data={historical_data} error={getting_data_error}/>
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
