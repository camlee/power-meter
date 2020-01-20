export function processLogResponse(response_text, offset){
  let data = [];
  const records = response_text.split("\n");
  for (const record of records){
    let values = record.split(",");
    values[0] = Number(values[0]) + offset;
    values[1] = Number(values[1]);
    values[2] = Number(values[2]);
    data.push(values);
  }
  return data;
}

export function processDataMeta(meta){
  let logs = [];
  for (const [log_number, log_meta] of Object.entries(meta["logs"])){
    logs.push({
      number: log_number,
      start_time_offset: log_meta.start_time_offset,
      start_time: log_meta.start_time,
    })
  }

  // Skip data that doesn't have complete time information:
  let original_log_file_count = logs.length;
  logs = logs.filter(function(a){
    return !(a.start_time_offset === null);
  });
  console.log(`Number of log files: ${original_log_file_count} -> ${logs.length}`);

  return logs;
}
