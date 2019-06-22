console.log("Hello World from Javascript!");

import Chartist from "chartist/dist/chartist.min.js";
import "../node_modules/chartist/dist/chartist.min.css";
import "./index.css";

new Chartist.Bar('#chart1', {
  labels: ['Sat', 'Sun', 'Mon', 'Today'],
  series: [
    [1, 2, 3, 4],
    [1.2, 1.3, 1.1, 1.35]
  ]
}, {
  stackBars: true,
  axisY: {
    type: Chartist.FixedScaleAxis,
    ticks: [0, 1, 2, 3, 4, 5],
    low: 0,
    labelInterpolationFnc: function(value) {
      return value + ' wH';
    }
  }
}).on('draw', function(data) {
  if(data.type === 'bar') {
    data.element.attr({
      style: 'stroke-width: 5em'
    });
  }
});

new Chartist.Bar('#chart2', {
  labels: ['9:00 AM', '10:00 AM', '11:00 AM', '12:00 PM', '1:00 PM', '2:00 PM'],
  series: [
    [4, 5, 5, 4, 5, 5]
  ]
}, {
  stackBars: true,
  axisY: {
    labelInterpolationFnc: function(value) {
      return value + ' wH';
    }
  }
})
// .on('draw', function(data) {
//   if(data.type === 'bar') {
//     data.element.attr({
//       style: 'stroke-width: 30px'
//     });
//   }
// });

