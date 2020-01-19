<script>
  import { onMount } from 'svelte';
  import LinearProgress from '@smui/linear-progress';

  export let getting_stats;

  let canvas;
  let stats = null;

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
            labels: ['Server Code', 'Client Code', 'Data', 'Free',],
            datasets: [{
                data: [0, 0, 0, 1],
                backgroundColor: [
                    '#2196f3',
                    '#ff9800',
                    '#90ee90',
                    '#f0f0f0',
                ],
            }]
        },
        options: {
          tooltips: {
            callbacks: {
              label: function(tooltipItem, data) {
                if (stats != null){
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
    chart.data.datasets[0].data = [
      stats.disk_usage.server,
      stats.disk_usage.static,
      stats.disk_usage.data,
      stats.disk_free,
      ];
      chart.update();
  }

  async function main(){
    const ctx = canvas.getContext('2d');
    let chart = createChart(ctx);
    stats = await getting_stats;
    updateChart(chart, stats);
  };

  onMount(main);

</script>

<h2>Disk Usage</h2>
<canvas bind:this={canvas}/>
{#await getting_stats}
  <LinearProgress indeterminate/>
{:then}
  <LinearProgress indeterminate closed={1}/>
{:catch error}
  <LinearProgress class="error-progress-bar" progress={1}/>
{/await}