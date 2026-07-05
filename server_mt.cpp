#include <sys/socket.h>
#include <netinet/in.h>
#include <unistd.h>
#include <memory.h>
#include <iostream>
#include <string>
#include <fcntl.h>
#include <sys/epoll.h>
#include <cerrno>
#include <memory>
#include <unordered_map>
#include <mutex>
#include <thread>
#include <vector>
#include <cstdlib>

std::unordered_map<std::string, std::string> store;
std::mutex store_mutex;
std::mutex cout_mutex;

struct Connection
{
    int fd;
    std::string inbuf;
    std::string outbuf;
};

void set_nonblocking(int fd)
{
    int flags = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}
void delete_socket(int epfd, Connection* c)
{
    epoll_ctl(epfd, EPOLL_CTL_DEL, c->fd, nullptr);
    close(c->fd);
    delete c;
}

std::string process(const std::string& line, std::unordered_map<std::string, std::string>& store)
{
    std::string cmd, key, value;

    if(line.empty()) return "ERR empty\n";
    
    size_t pos = line.find(" ");
    if(pos == std::string::npos) return "ERR need arguments\n";

    cmd = line.substr(0, pos);
    if(cmd == "GET" || cmd == "DEL")
    {
        key = line.substr(pos + 1);
    }
    else if(cmd == "SET")
    {
        size_t second_pos = line.find(" ", pos + 1);
        if(second_pos == std::string::npos) return "ERR SET needs key and value\n";

        key = line.substr(pos + 1, second_pos - pos - 1);
        value = line.substr(second_pos + 1);
    }
    else
    {
        return "ERR unknown command\n";
    }

    if(cmd == "SET")
    {
        std::lock_guard<std::mutex> g(store_mutex);
        store[key] = value;
        return "OK\n";
    }
    else if(cmd == "GET")
    {
        std::lock_guard<std::mutex> g(store_mutex);
        auto it = store.find(key);

        return it != store.end() ? "value: " + it->second + "\n" : "value: NOT FOUND\n";
   }
    else if(cmd == "DEL")
    {
        std::lock_guard<std::mutex> g(store_mutex);

        return store.erase(key) == 1 ? "OK\n" : "NOT FOUND\n";
    }

    return "ERR";
}

void print_worker_id(int worker_id)
{
    std::lock_guard<std::mutex> g(cout_mutex);
    std::cerr << "worker " << worker_id << " started\n";
}

void worker(int worker_id)
{
    int socketNum = socket(AF_INET, SOCK_STREAM, 0);
    if(socketNum < 0)
    {
        std::perror("socket"); 
        std::exit(1);
    }
    set_nonblocking(socketNum);

    std::unique_ptr<Connection> listener = std::make_unique<Connection>(Connection{socketNum, "", ""});

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(8080);

    int opt = 1;
    setsockopt(listener->fd, SOL_SOCKET, SO_REUSEPORT, &opt, sizeof(opt));
    setsockopt(listener->fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    
    print_worker_id(worker_id);

    int bindResult = bind(listener->fd, (struct sockaddr*)&addr, sizeof(addr));
    
    if(bindResult < 0)
    {
        std::perror("bind");
        std::exit(1);
    }

    int backlog = SOMAXCONN;
    
    if(listen(listener->fd, backlog) == -1)
    {
        std::perror("listen");
        std::exit(1);
    }
    
    int flags = 0; 
    int epfd = epoll_create1(flags);
    if(epfd == -1)
    {
        std::perror("epoll_create");
        std::exit(1);
    }

    struct epoll_event ev;
    ev.events = EPOLLIN;
    ev.data.ptr = listener.get();
    int ctl = epoll_ctl(epfd, EPOLL_CTL_ADD, listener->fd, &ev);
    if(ctl == -1)
    {
        std::perror("main ctl_add");
        std::exit(1);
    }

    while(true)
    {
        struct epoll_event events[64];
        int nready = epoll_wait(epfd, events, 64, -1);
        for(int i = 0; i < nready; i++)
        {
            Connection* c = (Connection*)events[i].data.ptr;
            if(c->fd == listener->fd)
            {
                //*accept*
                while(true)
                {
                    int client = accept(listener->fd, nullptr, 0);
                    if(client < 0)
                    {
                        if(errno == EAGAIN || errno == EWOULDBLOCK) break;
                        std::perror("accept");
                        break;
                    }
                    set_nonblocking(client);
                    
                    Connection* conn = new Connection{client, "", ""};
                    struct epoll_event cev;
                    cev.events = EPOLLIN;
                    cev.data.ptr = conn;
                    if(epoll_ctl(epfd, EPOLL_CTL_ADD, conn->fd, &cev) == -1)
                    {
                        std::perror("accept epoll_ctl");
                        delete_socket(epfd, conn);
                    }
                }
            }
            else {
                //*read*write*close*
                char tmp[4096];
                int n;
                bool closed = false;

                while(true)
                {
                    n = read(c->fd, tmp, sizeof(tmp));
                    if(n > 0) c->inbuf.append(tmp, n);
                    if(n == 0) 
                    {
                        closed = true;
                        delete_socket(epfd, c);
                        break; //and delete
                    }
                    if(n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) break;
                    if(n < 0)
                    {
                        std::perror("read");
                        closed = true;
                        delete_socket(epfd, c);
                        break;//and delete
                    }
                }
                if(closed) continue;

                while(true)
                {
                    size_t pos = c->inbuf.find('\n');
                    if(pos == std::string::npos) break;

                    std::string line = c->inbuf.substr(0, pos);
                    c->inbuf.erase(0, pos + 1);
                    
                    std::string response = process(line, store);
                    c->outbuf += response;
                }

                while(c->outbuf.size() > 0)
                {
                    n = write(c->fd, c->outbuf.data(), c->outbuf.size());
                    if(n > 0)
                    {
                        c->outbuf.erase(0, n);
                    }
                    if(n < 0 && errno == EAGAIN) break;
                    if(n < 0)
                    {
                        std::perror("write");
                        delete_socket(epfd, c);
                        break;
                    }
                }
            }
        }
    }

}

int main()
{
    unsigned N = std::thread::hardware_concurrency();
    if(N == 0) N = 4;

    std::cout << "listening on 8080..." << std::endl;
    
    std::vector<std::thread> workers;
    for(unsigned i = 0; i < N; i++)
    {
        workers.emplace_back(worker, i);
    }
    
    for(auto& t : workers)
    {
        t.join();
    }

    return 0;
}
