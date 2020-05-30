<script>
  import { onMount } from 'svelte';
  import LinearProgress from '@smui/linear-progress';

  export let getting_stats;

  let canvas;
  let stats = null;

  let still_getting_stats = true;
  let error_getting_stats = false;

  function formatBytes(bytes, decimals=2) {
    if (bytes === 0) return '0 Bytes';

    const k = 1024;
    const dm = decimals < 0 ? 0 : decimals;
    const sizes = ['Bytes', 'KB', 'MB', 'GB', 'TB', 'PB', 'EB', 'ZB', 'YB'];

    const i = Math.floor(Math.log(bytes) / Math.log(k));

    return parseFloat((bytes / Math.pow(k, i)).toFixed(dm)) + ' ' + sizes[i];
  }

  function createChart(ctx){
    let chart = new Chart(ctx, {
        type: 'pie',
        data: {
            labels: ['Unknown'],
            datasets: [{
                data: [1],
            }]
        },
        options: {
          tooltips: {
            callbacks: {
              label: function(tooltipItem, data) {
                if (stats == null){
                  return "100% Unknown";
                } else {
                  let allData = data.datasets[tooltipItem.datasetIndex].data;
                  let tooltipLabel = data.labels[tooltipItem.index];
                  let tooltipData = allData[tooltipItem.index];
                  let tooltipPercentage = ((tooltipData / stats.disk_size) * 100).toFixed(1);
                  return tooltipLabel + ': ' + formatBytes(tooltipData, 1) + ' (' + tooltipPercentage + '%)';
                }
              }
            }
          }
        }
    });
    return chart;
  }

  function updateChart(chart, stats){
    chart.data.labels = ['Server Code', 'Client Code', 'Data', 'Free',];
    chart.data.datasets = [{
      data: [
        stats.disk_usage.server,
        stats.disk_usage.static,
        stats.disk_usage.data,
        stats.disk_free,
        ],
      backgroundColor: [
        '#2196f3',
        '#ff9800',
        '#90ee90',
        '#f0f0f0',
        ],
      }];
    chart.update();
  }

  async function main(){
    getting_stats.finally(function() {still_getting_stats = false});
    getting_stats.catch(function() {error_getting_stats = true});
    let chart = createChart(canvas.getContext('2d'));
    try{
      stats = await getting_stats;
    } catch(e){
      return; // Just leaving the chart as-is is fine. The Progress bar will show there's an error.
    }
    updateChart(chart, stats);
  };

  onMount(main);

</script>

<h2>Disk Usage</h2>
<canvas bind:this={canvas}/>
<LinearProgress
  indeterminate
  closed={!still_getting_stats && !error_getting_stats}
  progress={error_getting_stats}
  class={error_getting_stats ? "error-progress-bar" : ""}
  />