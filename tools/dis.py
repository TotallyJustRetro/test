import sys; sys.path.insert(0,'.')
import sm83
rom=open(sys.argv[1],'rb').read()
def addr(a,bank): return a if a<0x4000 else bank*0x4000+(a-0x4000)
s=int(sys.argv[2],16); e=int(sys.argv[3],16); bank=int(sys.argv[4]) if len(sys.argv)>4 else 1
pc=s
while pc<e:
    o=addr(pc,bank); op=rom[o]; b1=rom[o+1]; b2=rom[o+2]
    if op==0xCB: i=sm83.decode(0xCB,b1,0,pc)
    else: i=sm83.decode(op,b1,b2,pc)
    print(f"{pc:04X}: {' '.join('%02X'%rom[o+k] for k in range(i.length)):9} {i.text}")
    pc+=i.length
