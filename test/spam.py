import argparse
import socket


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('-p', '--port', type=int, default=3004)
    parser.add_argument('-n', '--num-stats', type=int, default=10)
    parser.add_argument('-r', '--reconnect-interval', type=int, default=0)
    parser.add_argument('--word-file')
    args = parser.parse_args()

    sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    sock.connect(('localhost', args.port))

    reconn = args.reconnect_interval
    count = args.num_stats
    words = []
    if args.word_file:
        with open(args.word_file) as wf:
            x = 0
            for line in wf:
                words.append(line.strip())
                x += 1
        if count == 0:
            count = x
    else:
        words = ['test']

    x = 0
    while True:
        break_out = False
        for word in words:
            x += 1
            print x, count
            stat = word + ':1|c\n'
            print("sending {0}".format(stat));
            sock.sendall(stat)
            if count and x >= count:
                break_out = True
                break
            elif reconn and x % reconn == 0:
                sock.close()
                sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
                sock.connect(('localhost', args.port))
        if break_out:
            break
