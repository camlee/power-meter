<script>
  import DiskUsage from './DiskUsage.svelte';
  import FullPageProgress from './FullPageProgress.svelte';
  import "@material/layout-grid/mdc-layout-grid";


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

  let deferreds = [];
  let getting_stats = new Promise(function(resolve, reject){
    deferreds.push({resolve: resolve, reject: reject});
  });

  async function get_stats(){
    let response = await fetch(API_URL + '/stats');
    console.log(response.status);
    let stats = await response.json();
    deferreds[0].resolve(stats);
  }

  async function main(){
    try {
      var response = await fetch(API_URL + "/set_time?" + getTimeParameter(), {"method": "POST"});
    } catch(e){
      loading_error = e;
      return;
    }
    if (response.status != 202){
      loading_error = "Couldn't set the time.";
      return;
    }

    loading_verb = "Loading";
    loading_action = "Load";
    try {
      var response = await import('chart.js');
    } catch(e){
      loading_error = e;
      return;
    }
    loading = false;
    await get_stats();
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
            <h2>Today's Power Usage</h2>
            <canvas id="today_power"/>
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
