// A device command and optional playlist edits commit in the same CONTROL_DB transaction.
export async function commitTarget(env,row,baseRevision,desired,{albumRevision,edits=()=>[],refresh=true,cooldownMs=0}={}) {
  const id=crypto.randomUUID(),now=Date.now(),db=env.CONTROL_DB;
  const guard=albumRevision===undefined?'':' AND EXISTS(SELECT 1 FROM md_album_state WHERE device_id=md_devices.id AND revision=?)';
  const cooldown=cooldownMs?' AND requested_at<=?':'';
  const args=[JSON.stringify(desired),id,now,row.id,'personal',baseRevision,...(albumRevision===undefined?[]:[albumRevision]),...(cooldownMs?[now-cooldownMs]:[])];
  const result=await db.batch([
    db.prepare(`UPDATE md_devices SET revision=revision+${refresh?1:0},desired_json=?,command_id=?,requested_at=? WHERE id=? AND space_id=? AND enabled=1 AND revision=? AND revision<4294967295${guard}${cooldown} RETURNING *`).bind(...args),
    ...edits(id),
    ...(refresh?[db.prepare('INSERT INTO md_commands(device_id,revision,desired_json,created_at) SELECT id,revision,desired_json,? FROM md_devices WHERE id=? AND command_id=?').bind(now,row.id,id),
    db.prepare('DELETE FROM md_commands WHERE device_id=? AND revision<(SELECT revision-31 FROM md_devices WHERE id=? AND command_id=?)').bind(row.id,row.id,id)]:[])
  ]);
  return result[0].results[0]||null;
}
