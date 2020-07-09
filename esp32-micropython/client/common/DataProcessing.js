export function processLogResponse(response_text, offset, min_time){
  let data = [];
  const records = response_text.split("\n");
  for (const record of records){
    let values = record.split(",");
    if (values.length < 3){
      continue;
    }
    values[0] = Number(values[0]) + offset;
    values[1] = Number(values[1]);
    values[2] = Number(values[2]);
    if (min_time === undefined || min_time === null || values[0] > min_time){
      data.push(values);
    }
  }
  return data;
}

export function processDataMeta(meta){
  let logs = [];
  for (const record of meta.split("\n")){
    let values = record.split(",");
    if (record.length < 4){
      continue;
    }
    let start_time_offset = Number(values[3]);
    if (isNaN(start_time_offset)){
      start_time_offset = null;
    }
    logs.push({
      number: Number(values[0]),
      active: Boolean(Number(values[1])),
      start_time: Number(values[2]),
      start_time_offset: start_time_offset,
    })
  }
  return logs;
}

export async function readLogResponseWithProgress(response, update_progress){
  // https://javascript.info/fetch-progress

  // Step 1: obtain a reader from the fetch response
  const reader = response.body.getReader();

  // Step 2: get total length
  const contentLength = +response.headers.get('Content-Length');

  // Step 3: read the data
  let receivedLength = 0; // received that many bytes at the moment
  let chunks = []; // array of received binary chunks (comprises the body)
  while(true) {
    const {done, value} = await reader.read();

    if (done) {
      break;
    }

    chunks.push(value);
    receivedLength += value.length;

    // console.log(`Received ${receivedLength} of ${contentLength} (${receivedLength / contentLength * 100} %)`);
    if (update_progress !== undefined && contentLength > 0){
      update_progress(receivedLength / contentLength);
    }
  }

  if (update_progress !== undefined){
    update_progress(1);
  }

  // Step 4: concatenate chunks into single Uint8Array
  let chunksAll = new Uint8Array(receivedLength); // (4.1)
  let position = 0;
  for(let chunk of chunks) {
    chunksAll.set(chunk, position); // (4.2)
    position += chunk.length;
  }

  // Step 5: decode into a string
  let result = new TextDecoder("utf-8").decode(chunksAll);

  // We're done!
  return result;
}