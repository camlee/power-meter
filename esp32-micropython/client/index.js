var API_URL = process.env.API_URL;

function formatBytes(bytes, decimals=2) {
    if (bytes === 0) return '0 Bytes';

    const k = 1024;
    const dm = decimals < 0 ? 0 : decimals;
    const sizes = ['Bytes', 'KB', 'MB', 'GB', 'TB', 'PB', 'EB', 'ZB', 'YB'];

    const i = Math.floor(Math.log(bytes) / Math.log(k));

    return parseFloat((bytes / Math.pow(k, i)).toFixed(dm)) + ' ' + sizes[i];
}

async function drawDataUsage(){
  var response = await fetch(API_URL + 'stats');
  var stats = await response.json();

  var ctx = document.getElementById('disk_usage_chart').getContext('2d');
  var myChart = new Chart(ctx, {
      type: 'pie',
      data: {
          labels: ['Server Code', 'Client Code', 'Data', 'Free',],
          datasets: [{
              data: [
                stats.disk_usage.server,
                stats.disk_usage.static,
                stats.disk_usage.data,
                stats.disk_free,],
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
              var allData = data.datasets[tooltipItem.datasetIndex].data;
              var tooltipLabel = data.labels[tooltipItem.index];
              var tooltipData = allData[tooltipItem.index];
              var tooltipPercentage = ((tooltipData / stats.disk_size) * 100).toFixed(1);
              return tooltipLabel + ': ' + formatBytes(tooltipData, 1) + ' (' + tooltipPercentage + '%)';
            }
          }
        }
      }
  });
}

async function main(){
  var time = ((new Date).getTime() / 1000).toFixed(); // Unix epoch time in seconds
  fetch(API_URL + "set_time?time=" + time, {"method": "POST"});

  drawDataUsage();
}

console.log('Loading...');
import('chart.js').then(function(Chart){
  console.log('ready.');
  main();
});