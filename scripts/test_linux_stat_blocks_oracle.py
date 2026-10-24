"""Offline ext4 allocation oracle; never mounts a host filesystem."""
from pathlib import Path
import argparse,hashlib,json,shutil,subprocess,os,re,struct,sys
from linux_stat_test_support import build_model
p=argparse.ArgumentParser()
p.add_argument('--output',type=Path,required=True)
p.add_argument('--sanitize',action='store_true')

a=p.parse_args()
root=Path(__file__).resolve().parents[1]
out=a.output.resolve();out.mkdir(parents=True,exist_ok=False)
sha=lambda p:hashlib.sha256(Path(p).read_bytes()).hexdigest()
source,binary,files,tools,model_receipt=build_model(root,out,'raw-blocks',a.sanitize)
commands=[];toolset={}
def run(argv,label,cwd=None):
 argv=list(map(str,argv));exe=Path(shutil.which(argv[0])).resolve();toolset[str(exe)]=sha(exe)
 result=subprocess.run(argv,cwd=cwd,env={**os.environ,'XMAKE_ROOT':'y'},stdout=subprocess.PIPE,stderr=subprocess.STDOUT,text=True)
 log=out/(label+'.log');log.write_text(result.stdout)
 commands.append(dict(argv=argv,returncode=result.returncode,log_sha256=sha(log)))
 (out/'commands.json').write_text(json.dumps(commands,indent=2)+'\n');assert result.returncode==0,(argv,log)
 return result.stdout
rows=[]
for block in (1024,2048,4096):
 for stride in (128,256):
  label=f'{block}-{stride}';folder=out/label;folder.mkdir();disk=folder/'input.img'
  run(['mke2fs','-q','-t','ext4','-b',block,'-I',stride,'-O','none,extent,filetype,sparse_super,large_file','-E','lazy_itable_init=0','-m','0',disk,'16M'],label+'-mkfs')
  (folder/'empty').write_bytes(b'');(folder/'dense').write_bytes(b'x'*(8*block));(folder/'sparse').write_bytes(b'\0'*(4*block)+b'x'*block)
  cmds=[f'write {folder}/empty /empty',f'write {folder}/dense /four',f'write {folder}/sparse /sparse','mkdir /dir','symlink /fast short-target','symlink /block '+('a'*80)]
  cmds += [f'punch /four {i} {i}' for i in (1,3,5,7)]
  command_file=folder/'debugfs.commands';command_file.write_text('\n'.join(cmds)+'\n')
  run(['debugfs','-w','-f',command_file,disk],label+'-populate')
  run(['e2fsck','-fn',disk],label+'-fsck')
  data=disk.read_bytes();superblock=data[1024:2048]
  u16=lambda b,o:struct.unpack_from('<H',b,o)[0]
  u32=lambda b,o:struct.unpack_from('<I',b,o)[0]
  first=u32(superblock,20);blocks=u32(superblock,4);per_group=u32(superblock,32);ipg=u32(superblock,40);inodes=u32(superblock,0)
  groups=(blocks-first+per_group-1)//per_group;gd=data[(first+1)*block:];reserved=[(0,first)]
  tables=[]
  def power(number,base):
   if number<1:return False
   while number%base==0:number//=base
   return number==1
  for group in range(groups):
   start=first+group*per_group
   if group in (0,1) or any(power(group,b) for b in (3,5,7)):reserved.append((start,start+1+(groups*32+block-1)//block))
   desc=gd[group*32:group*32+32];bitmap=u32(desc,0);imap=u32(desc,4);table=u32(desc,8);tables.append(table)
   reserved += [(bitmap,bitmap+1),(imap,imap+1),(table,table+(ipg*stride+block-1)//block)]
  ranges=folder/'metadata-ranges.txt';ranges.write_text(''.join(f'{a} {b}\n' for a,b in reserved))
  for name in ('empty','four','sparse','dir','fast','block'):
   text=run(['debugfs','-R','stat /'+name,disk],label+'-stat-'+name)
   ino=int(re.search(r'Inode:\s*(\d+)',text)[1]);expected=int(re.search(r'Blockcount:\s*(\d+)',text)[1])
   at=tables[(ino-1)//ipg]*block+((ino-1)%ipg)*stride;raw=folder/(name+'.inode');raw.write_bytes(data[at:at+stride])
   run([binary,raw,block,blocks,expected,ranges],label+'-query-'+name)
   rows.append(dict(block_size=block,inode_size=stride,name=name,ino=ino,blocks512=expected,raw_sha256=sha(raw),disk_sha256=sha(disk)))
for name,h in files.items():assert sha(source/name)==h
receipt=dict(status='pass',scope='Hosted pure raw-inode decoder against e2fsprogs-created images; no provider/guest/mount execution',source_sha256=files,model_receipt_sha256=sha(model_receipt),toolset={**tools,**toolset},binary_sha256=sha(binary),rows=rows,commands=commands)
(out/'result.json').write_text(json.dumps(receipt,indent=2)+'\n');print('PASS',len(rows),'raw image nodes')
