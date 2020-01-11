<script>
  import DiskUsage from './DiskUsage.svelte';


  let API_URL = process.env.API_URL || "http://" + location.host;
  let WS_URL = process.env.WS_URL || "ws://" + location.host;

  function getTimeParameter(){
    let time = ((new Date()).getTime() / 1000).toFixed(); // Unix epoch time in seconds
    return "time=" + time;
  }


  let setting_time = fetch(API_URL + "/set_time?" + getTimeParameter(), {"method": "POST"});
  let loading_chartjs = import('chart.js');

  let loading = Promise.all([setting_time, loading_chartjs]);
  let deferreds = [];
  let getting_stats = new Promise(function(resolve, reject){
    deferreds.push({resolve: resolve, reject: reject});
  });

  async function get_stats(){
    let response = await fetch(API_URL + '/stats');
    let stats = await response.json();
    deferreds[0].resolve(stats);
  }

  async function main(){
    await loading;
    get_stats();
  }

  main();
</script>

<main>
{#await loading}
  <p>loading...</p>
{:then }
  <h1>Power Meter</h1>
    <div class="row">
        <div class="col m6">
            <h2>Realtime</h2>
            <canvas id="realtime"/>
        </div>
        <div class="col m6">
            <h2>Today's Power Usage</h2>
            <canvas id="today_power"/>
        </div>
    </div>
    <div class="row">
        <div class="col m6">
          <DiskUsage getting_stats={getting_stats}/>
        </div>
    </div>
{:catch error}
  <p style="color: red">Uhh Ohhh. Something went wrong:</p>
  <p style="color: red">{error.message}</p>
{/await}

</main>
