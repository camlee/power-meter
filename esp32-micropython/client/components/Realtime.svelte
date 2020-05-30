<script>
  import { onMount } from 'svelte';
  import LinearProgress from '@smui/linear-progress';

  export let minutes_to_show = 2;
  export let max_minutes_to_show = 10;
  export let websocket = null;

  let canvas;
  let chart = null;
  let error = false;
  let data_received_recently = false;
  let last_data_received_at = null;

  $: setup_websocket(websocket, chart); // Re-setup the websocket whenever it changes.

  async function createChart(ctx){
    let min_time = new Date();
    min_time.setSeconds(0);

    let chart = new Chart(ctx, {
      type: 'line',
      data: {
        datasets: [{
          label: "In",
          data: [],
          fill: false,
          borderColor: "blue",
          backgroundColor: "blue",
        }, {
          label: "Out",
          data: [],
          fill: false,
          borderColor: "orange",
          backgroundColor: "orange",
        }]
      },
      options: {
        tooltips: {
          enabled: false,
        },
        scales: {
          xAxes: [{
            type: "time",
            position: "bottom",
            time: {
              unit: 'minute',
              stepSize: 1,
            },
          }],
          yAxes: [{
            scaleLabel: {
              display: true,
              labelString: "Power [W]"
            },
            ticks: {
              beginAtZero: true
            }
          }]
        }
      }
    });

    function adjustAxisForTimeProgression(chart, minutes_to_show){
      let ticks = chart.options.scales.xAxes[0].ticks;
      ticks.max = new Date();
      ticks.min = new Date(ticks.max.getTime() - minutes_to_show*60000);
      chart.update();
    }

    setInterval(function() {adjustAxisForTimeProgression(chart, minutes_to_show);}, 50);

    return chart;
  }

  async function processData(data){
    let records = data.split("\n");

    let in_power = [];
    let out_power = [];

    for (const record of records){
      let values = record.split(",");
      if (values[0]) {
        let epoch_time = Number(values[0]);
        let time = new Date(epoch_time);

        in_power.push({x: time, y: Number(values[1])});
        out_power.push({x: time, y: Number(values[2])});
      }
    }

    return [in_power, out_power];
  }

  function addToRecentData(data, new_data){
    data.push(...new_data);

    // Removing data that won't be shown anymore:
    let current_time = new Date();
    while (data.length > 0 && (current_time - data[0].x.getTime())/1000/60 > max_minutes_to_show){
      data.shift();
    }
  }

  async function updateChart(chart, in_power, out_power){
    addToRecentData(chart.data.datasets[0].data, in_power);
    addToRecentData(chart.data.datasets[1].data, out_power);
    chart.update(0);
  }

  function setup_websocket(socket, chart){
    if (socket === null){
      return; // No websocket: nothing to set up.
    }

    socket.addEventListener("message", async function(event){
      // console.log(`Web socket data received: ${data}`);
      error = false;
      data_received_recently = true;
      last_data_received_at = performance.now();

      if (chart === null) {
        return; // Chart not ready yet: nothing to do.
      }

      let [in_power, out_power] = await processData(event.data);
      updateChart(chart, in_power, out_power);
    });

    socket.addEventListener("close", function(event){
      error = true;
    });

    socket.addEventListener("error", function(event){
      error = true;
    });
  }

  function monitor_websocket(){
    setInterval(function(){
      if (performance.now() - last_data_received_at > 5000){
        data_received_recently = false;
      }
    }, 200);
  }

  // var socket = new WebSocket(WS_URL);
  // socket.onopen = function(e) {
  //   console.log(`Web socket connection established`);
  //   socket.send(getTimeParameter());
  // };

  // socket.onmessage = async function(event) {
  //   var data = event.data;
  //   // console.log(`Web socket data received: ${data}`);
  //   var [in_power, out_power] = await processData(data);
  //   updateChart(chart, in_power, out_power);
  // };

  // socket.onclose = function(event) {
  //   if (event.wasClean) {
  //     console.log(`Web socket connection closed cleanly, code=${event.code} reason=${event.reason}`);
  //   } else {
  //     // e.g. server process killed or network down
  //     // event.code is usually 1006 in this case
  //     console.log(`Web socket connection died`);
  //   }
  // };

  // socket.onerror = function(error) {
  //   console.log(`Web socket error=${error.message}`);
  // };

  async function main(){
    monitor_websocket();
    chart = await createChart(canvas.getContext('2d'));
  }

  onMount(main);

</script>


<h2>Realtime</h2>
<canvas bind:this={canvas}/>
<LinearProgress
  indeterminate
  closed={data_received_recently && !error && WebSocket !== null}
  class={error ? "error-progress-bar" : ""}
  />