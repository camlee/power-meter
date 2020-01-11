var API_URL = process.env.API_URL || "http://" + location.host;
var WS_URL = process.env.WS_URL || "ws://" + location.host;

function formatBytes(bytes, decimals=2) {
    if (bytes === 0) return '0 Bytes';

    const k = 1024;
    const dm = decimals < 0 ? 0 : decimals;
    const sizes = ['Bytes', 'KB', 'MB', 'GB', 'TB', 'PB', 'EB', 'ZB', 'YB'];

    const i = Math.floor(Math.log(bytes) / Math.log(k));

    return parseFloat((bytes / Math.pow(k, i)).toFixed(dm)) + ' ' + sizes[i];
}

function getTimeParameter(){
    var time = ((new Date()).getTime() / 1000).toFixed(); // Unix epoch time in seconds
    return "time=" + time;
}

async function drawRealtime(){
  var minutes_to_show = 2;
  var max_minutes_to_show = 10;

  var min_time = new Date();
  min_time.setSeconds(0);

  var ctx = document.getElementById('realtime').getContext('2d');
  var chart = new Chart(ctx, {
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
    var ticks = chart.options.scales.xAxes[0].ticks;
    ticks.max = new Date();
    ticks.min = new Date(ticks.max.getTime() - minutes_to_show*60000);
    chart.update();
  }

  setInterval(function() {adjustAxisForTimeProgression(chart, minutes_to_show);}, 50);

  async function processData(data){
    var records = data.split("\n");

    var in_power = [];
    var out_power = [];

    for (const record of records){
      values = record.split(",");
      if (values[0]) {
        var epoch_time = Number(values[0]);
        var time = new Date(epoch_time);

        in_power.push({x: time, y: Number(values[1])});
        out_power.push({x: time, y: Number(values[2])});
      }
    }

    return [in_power, out_power];
  }

  function addToRecentData(data, new_data){
    data.push(...new_data);

    // Removing data that won't be shown anymore:
    var current_time = new Date();
    while (data.length > 0 && (current_time - data[0].x.getTime())/1000/60 > max_minutes_to_show){
      data.shift();
    }
  }

  async function updateChart(chart, in_power, out_power){
    addToRecentData(chart.data.datasets[0].data, in_power);
    addToRecentData(chart.data.datasets[1].data, out_power);
    chart.update(0);
  }

  var socket = new WebSocket(WS_URL);
  socket.onopen = function(e) {
    console.log(`Web socket connection established`);
    socket.send(getTimeParameter());
  };

  socket.onmessage = async function(event) {
    var data = event.data;
    // console.log(`Web socket data received: ${data}`);
    var [in_power, out_power] = await processData(data);
    updateChart(chart, in_power, out_power);
  };

  socket.onclose = function(event) {
    if (event.wasClean) {
      console.log(`Web socket connection closed cleanly, code=${event.code} reason=${event.reason}`);
    } else {
      // e.g. server process killed or network down
      // event.code is usually 1006 in this case
      console.log(`Web socket connection died`);
    }
  };

  socket.onerror = function(error) {
    console.log(`Web socket error=${error.message}`);
  };

}

async function drawPowerUsage(){
  function processLogResponse(response_text, offset){
    var data = [];
    var records = response_text.split("\n");
    for (const record of records){
      var values = record.split(",");
      values[0] = Number(values[0]) + offset;
      values[1] = Number(values[1]);
      values[2] = Number(values[2]);
      data.push(values);
    }
    return data;
  }

  async function* getData(){
    var response = await fetch(API_URL + '/data/meta.json');
    var meta = await response.json();

    var logs = [];
    for (const [log_number, log_meta] of Object.entries(meta["logs"])){
      logs.push({
        number: log_number,
        start_time_offset: log_meta.start_time_offset,
        start_time: log_meta.start_time,
      })
    }
    console.log("Number of log files:");
    console.log(logs.length);


    // Skip data that doesn't have complete time information:
    logs = logs.filter(function(a){
      return !(a.start_time_offset === null);
    });

    // Load the newest data first as that's most likely to be viewed first:
    logs.sort(function(a, b) {
      var a_value = a.start_time_offset + a.start_time;
      var b_value = b.start_time_offset + b.start_time;
      return b_value - a_value;
    })

    // Download the data:
    for (const log of logs){
      console.log(`Downloading ${log.number}.csv (${new Date(log.start_time_offset + log.start_time)})`);

      var response = await fetch(API_URL + '/data/' + log.number + '.csv');
      var response_text = await response.text();
      yield await processLogResponse(response_text, log.start_time_offset);
    }
  }

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

    energy_chart = chart;

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

  function updateChart(chart, in_energy, out_energy){
    updateDataInPlace(chart.data.datasets[0].data, in_energy);
    updateDataInPlace(chart.data.datasets[1].data, out_energy);
    chart.update();
  }

  var ctx = document.getElementById('today_power').getContext('2d');
  var chart = createChart(ctx);

  var all_data = [];


  for await (const data of getData()) {
    all_data.push(...data);
    var [in_energy, out_energy] = processData(all_data, bucketize_15_min);
    updateChart(chart, in_energy, out_energy)
  }

  console.log("Done updating chart with with all the data!");

}

async function drawDiskUsage(){
  var response = await fetch(API_URL + '/stats');
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
  await fetch(API_URL + "/set_time?" + getTimeParameter(), {"method": "POST"});

  drawRealtime();
  drawDiskUsage();
  drawPowerUsage();
}

console.log('Loading...');
import('chart.js').then(function(Chart){
  console.log('ready.');
  main();
});