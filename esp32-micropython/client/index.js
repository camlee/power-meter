var API_URL = process.env.API_URL;

function formatBytes(bytes, decimals=2) {
    if (bytes === 0) return '0 Bytes';

    const k = 1024;
    const dm = decimals < 0 ? 0 : decimals;
    const sizes = ['Bytes', 'KB', 'MB', 'GB', 'TB', 'PB', 'EB', 'ZB', 'YB'];

    const i = Math.floor(Math.log(bytes) / Math.log(k));

    return parseFloat((bytes / Math.pow(k, i)).toFixed(dm)) + ' ' + sizes[i];
}

function getTimeParameter(){
    var time = ((new Date).getTime() / 1000).toFixed(); // Unix epoch time in seconds
    return "time=" + time;
}

async function drawDiskUsage(){
  var response = await fetch(API_URL + 'stats?' + getTimeParameter());
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

async function drawRealtime(){
  var response = await fetch(API_URL + 'data/meta.json');
  var meta = await response.json();

  // Finding the newest log:
  var newest_log_key = null;
  var newest_log_value = null;
  var newest_time = null;
  for (const [key, value] of Object.entries(meta["logs"])){
    if (newest_time === null || value["start_time_offset"] > newest_time){
      newest_time = value["start_time_offset"];
      newest_log_key = key;
      newest_log_value = value;
    }
  }

  var response = await fetch(API_URL + 'data/' + newest_log_key + '.csv');
  var data = await response.text();
  var records = data.split("\n");
  // var time_data = [];
  // var voltage_data = [];
  // var current_data = [];

  var in_power = [];
  var out_power = [];

  for (const record of records){
    values = record.split(",");
    if (values[0]) {
      var epoch_time = Number(values[0]) + newest_log_value["start_time"] + newest_log_value["start_time_offset"];
      var time = new Date(epoch_time * 1000);
      var in_voltage = Number(values[1]);
      var in_current = Number(values[2]);
      var out_voltage = Number(values[3]);
      var out_current = Number(values[4]);

      in_power.push({x: time, y: in_voltage * in_current});
      out_power.push({x: time, y: out_voltage * out_current});
    }
  }

  var ctx = document.getElementById('realtime').getContext('2d');
  var myChart = new Chart(ctx, {
      type: 'line',
      data: {
          datasets: [{
            label: "In",
            data: in_power,
            fill: false,
            borderColor: "blue",
            backgroundColor: "blue",
          }, {
            label: "Out",
            data: out_power,
            fill: false,
            borderColor: "orange",
            backgroundColor: "orange",
          }]
      },
      options: {
        scales: {
          xAxes: [{
            type: "time",
            position: "bottom"
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
}


async function main(){
  // fetch(API_URL + "set_time?" + getTimeParameter(), {"method": "POST"});

  drawDiskUsage();
  drawRealtime();
}

console.log('Loading...');
import('chart.js').then(function(Chart){
  console.log('ready.');
  main();
});