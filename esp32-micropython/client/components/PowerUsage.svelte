<script>
  import { onMount } from 'svelte';
  import Chip, {Set, Icon, Text} from '@smui/chips';
  import LinearProgress from '@smui/linear-progress';
  import IconButton from '@smui/icon-button';
  import Refresh from "svelte-material-icons/Refresh.svelte";

  export let data = [];
  export let progress = false;
  export let error = null;
  export let refresh_data = function(){};

  let time_choice = "Today";

  let canvas;
  let chart;

  function round_for_graph(value){
    return Math.round(value * 10) / 10;
  }

  function processData(data, bucketize){
    let buckets = {};
    let panel_in = [];
    let panel_usage = [];
    let surplus = [];
    let in_bat = [];
    let out_bat = [];

    for (const values of data){
      if (values[0] && values[1] && values[2]) {
        let time = new Date(values[0]);

        let bucket = bucketize(time);
        let bucket_value = buckets[bucket];
        if (typeof(bucket_value) == "undefined"){
          // console.log("Initializing bucket " + new Date(bucket));
          bucket_value = {"in_bat": 0, "out_bat": 0, "panel_in": 0, "panel_usage": 0, "surplus": 0};
          buckets[bucket] = bucket_value;
        }

        // Converting Watt-Seconds to Watt-Hours (Wh) as we accumulate:
        // Also converting energy over 15 minutes to average power: TODO: this needs to be made dynamic when the resolution can be changed.
        let energy_in = values[1] / 3600 * 4;
        let energy_out = values[2] / 3600 * 4;
        let energy_avail = values[3] / 3600 * 4;

        // Calculating each series, keeping in mind it's a stacked bar chart:
        bucket_value["surplus"] += (energy_avail - energy_in);
        let net_bat_val =  energy_in - energy_out;
        let in_bat_val = 0;
        let out_bat_val = 0;
        if (net_bat_val >= 0){
          in_bat_val = net_bat_val;
        } else {
          out_bat_val = net_bat_val;
        }
        bucket_value["in_bat"] += in_bat_val;
        bucket_value["out_bat"] += out_bat_val;
        bucket_value["panel_in"] += (energy_in - in_bat_val);
        bucket_value["panel_usage"] += (-energy_out - out_bat_val)

      }
    }

    for (const key of Object.keys(buckets).sort()){
      let time = new Date(Number(key));
      in_bat.push({x: time, y: round_for_graph(buckets[key]["in_bat"])});
      out_bat.push({x: time, y: round_for_graph(buckets[key]["out_bat"])});
      panel_in.push({x: time, y: round_for_graph(buckets[key]["panel_in"])});
      panel_usage .push({x: time, y: round_for_graph(buckets[key]["panel_usage"])});
      surplus.push({x: time, y: round_for_graph(buckets[key]["surplus"])});
    }

    return [in_bat, out_bat, panel_in, panel_usage, surplus];
  }

  function bucketize_15_min(value){
    let rounded_minutes = Math.floor(value.getMinutes() / 15) * 15;
    let bucket_date = new Date(value)
    bucket_date.setMinutes(rounded_minutes);
    bucket_date.setSeconds(0);
    bucket_date.setMilliseconds(0);
    return bucket_date.getTime();
  }

  function createChart(ctx){
    const [min_time, max_time] = calculateDateRange(time_choice);

    let chart = new Chart(ctx, {
      type: 'bar',
      data: {
        datasets: [
          {
            label: "Battery Charging",
            data: [],
            fill: false,
            borderColor: "lawngreen",
            backgroundColor: "lawngreen",
          },
          {
            label: "Battery Usage",
            data: [],
            fill: false,
            borderColor: "orangered",
            backgroundColor: "orangered",
          },
          {
            label: "Panel In",
            data: [],
            fill: false,
            borderColor: "blue",
            backgroundColor: "blue",
          },
          {
            label: "Panel Usage",
            data: [],
            fill: false,
            borderColor: "orange",
            backgroundColor: "orange",
          },
          {
            label: "Panel Surplus",
            data: [],
            fill: false,
            borderColor: "deepskyblue",
            backgroundColor: "deepskyblue",
          },
        ]
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
              labelString: "Average Power [W]"
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
      let found = false;
      for (let existing_value of existing_data){
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

  function updateChartData(data){
    if (chart !== undefined && data.length > 0){
      let datasets_data = processData(data, bucketize_15_min);
      for (const [i, dataset_data] of datasets_data.entries()){
        updateDataInPlace(chart.data.datasets[i].data, dataset_data);
      }
      chart.update();
    }
  }


  function calculateDateRange(time_choice){
    let min_time;
    let max_time;

    switch (time_choice){
      case "Today":
        min_time = new Date();
        min_time.setHours(0);
        min_time.setMinutes(0);
        min_time.setSeconds(0);
        min_time.setMilliseconds(0);

        max_time = new Date(min_time);
        max_time.setDate(min_time.getDate() + 1);
        return [min_time, max_time];

      case "Yesterday":
        min_time = new Date();
        min_time.setHours(0);
        min_time.setMinutes(0);
        min_time.setSeconds(0);
        min_time.setMilliseconds(0);
        min_time.setDate(min_time.getDate() - 1);

        max_time = new Date(min_time);
        max_time.setDate(min_time.getDate() + 1);
        return [min_time, max_time];

      case "Last 3 days":
        min_time = new Date();
        min_time.setHours(0);
        min_time.setMinutes(0);
        min_time.setSeconds(0);
        min_time.setMilliseconds(0);
        min_time.setDate(min_time.getDate() - 2);

        max_time = new Date(min_time);
        max_time.setDate(min_time.getDate() + 3);
        return [min_time, max_time];

      case "Last 7 days":
        min_time = new Date();
        min_time.setHours(0);
        min_time.setMinutes(0);
        min_time.setSeconds(0);
        min_time.setMilliseconds(0);
        min_time.setDate(min_time.getDate() - 6);

        max_time = new Date(min_time);
        max_time.setDate(min_time.getDate() + 7);
        return [min_time, max_time];

      default:
        min_time = new Date();
        min_time.setHours(0);
        min_time.setMinutes(0);
        min_time.setSeconds(0);
        min_time.setMilliseconds(0);

        max_time = new Date(min_time);
        max_time.setDate(min_time.getDate() + 1);
        return [min_time, max_time];
    }
  }

  function updateChartDateRange(time_choice){
    if (chart !== undefined){
      let ticks = chart.options.scales.xAxes[0].ticks;
      [ticks.min, ticks.max] = calculateDateRange(time_choice);

      chart.update();
    }
  }

  $: updateChartData(data); // Update the chart anytime data changes.
  $: updateChartDateRange(time_choice) // Update the axis date range anytime the selection changes.

  async function main(){
    chart = createChart(canvas.getContext('2d'));
  }

  onMount(main);

</script>
<h2>{(time_choice || "Today")}{(time_choice || "Today").endsWith("s") ? "'" : "'s"} Power Usage</h2>
<div style="display:flex;">
  <Set chips={['Last 7 days', 'Last 3 days', 'Yesterday', 'Today']} let:chip choice bind:selected={time_choice}>
    <Chip tabindex="0">{chip}</Chip>
  </Set>
  <!-- <IconButton class="material-icons" on:click={refresh_data}>refresh</IconButton> -->
  <IconButton style="margin-left:auto" on:click={refresh_data()}><Refresh/></IconButton>
</div>
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