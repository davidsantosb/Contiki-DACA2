import socket
UDP_IP = "fd00::212:4b00:615:aaee"
UDP_PORT = 4321
MESSAGE = "LRT"
print "UDP target IP:", UDP_IP
print "UDP target port:", UDP_PORT
print "message:", MESSAGE
sock = socket.socket(socket.AF_INET6, socket.SOCK_DGRAM) # UDP
#sock.bind(('', 2222))
sock.bind(('', 1234))
sock.sendto(MESSAGE, (UDP_IP, UDP_PORT))
