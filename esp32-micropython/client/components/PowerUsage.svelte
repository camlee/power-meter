<script>
  import { onMount } from 'svelte';
  import LinearProgress from '@smui/linear-progress';

  export let data = [];
  export let progress = false;
  export let error = null;

  let canvas;
  let chart;

  function processData(data, bucketize){
    var buckets = {};
    var in_energy = [];
    var out_energy = [];

    for (const values of data){
      if (values[0] && values[1] && values[2]) {
        var time = new Date(values[0]);

        var bucket = bucketize(time);
        var bucket_value = buckets[bucket];
        if (typeof(bucket_value) == "undefined"){
          // console.log("Initializing bucket " + new Date(bucket));
          bucket_value = {"in": 0, "out": 0};
          buckets[bucket] = bucket_value;
        }

        // Converting Watt-Seconds to Watt-Hours (Wh) as we accumulate:
        bucket_value["in"] += values[1] / 60;
        bucket_value["out"] += values[2] / 60;
      }
    }

    for (const key of Object.keys(buckets).sort()){
      var time = new Date(Number(key));
      in_energy.push({x: time, y: Math.round(buckets[key]["in"])});
      out_energy.push({x: time, y: Math.round(-buckets[key]["out"])});
    }

    return [in_energy, out_energy];
  }

  function bucketize_15_min(value){
    var rounded_minutes = Math.floor(value.getMinutes() / 15) * 15;
    var bucket_date = new Date(value)
    bucket_date.setMinutes(rounded_minutes);
    bucket_date.setSeconds(0);
    bucket_date.setMilliseconds(0);
    return bucket_date.getTime();
  }

  function createChart(ctx, in_energy, out_energy){
    var min_time = new Date();
    min_time.setHours(0);
    min_time.setMinutes(0);
    min_time.setSeconds(0);
    min_time.setMilliseconds(0);

    var max_time = new Date(min_time);
    max_time.setDate(min_time.getDate() + 1);


    var chart = new Chart(ctx, {
      type: 'bar',
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
        scales: {
          xAxes: [{
            type: "time",
            position: "bottom",
            stacked: true,
            gridLines: {
              offsetGridLines: false
            },
            time: {
              unit: 'hour',
              stepSize: 3,
            },
            ticks: {
              min: min_time,
              max: max_time,
            }
          }],
          yAxes: [{
            stacked: true,
            scaleLabel: {
              display: true,
              labelString: "Energy [Wh]"
            },
            ticks: {
              beginAtZero: true
            }
          }]
        }
      }
    });

    return chart;
  }

  function updateDataInPlace(existing_data, new_data){
    for (const new_value of new_data){
      var found = false;
      for (var existing_value of existing_data){
        if (existing_value.x.getTime() == new_value.x.getTime()){
          found = true;
          existing_value.y = new_value.y;
          break;
        }
      }
      if (!found){
        existing_data.push(new_value);
      }
    }
  }

  function updateChart(data){
    if (chart !== undefined && data.length > 0){
      let [in_energy, out_energy] = processData(data, bucketize_15_min);
      updateDataInPlace(chart.data.datasets[0].data, in_energy);
      updateDataInPlace(chart.data.datasets[1].data, out_energy);
      chart.update();
    }
  }

  $: updateChart(data); // Update the chart anytime data changes.

  async function main(){
    chart = createChart(canvas.getContext('2d'));
  }

  onMount(main);

</script>

<h2>Today's Power Usage</h2>
<canvas bind:this={canvas}/>

<LinearProgress
  indeterminate={progress === false || progress === null}
  closed={progress === false || progress === 1}
  progress={progress}
  class={error ? "error-progress-bar" : ""}
  />

{#if error}
  <p>Failed to load data: {error}</p>
{/if}