#include <algorithm>
#include <asm-generic/errno.h>
#include <cerrno>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <string>
#include <sys/poll.h>
#include <system_error>
#include <thread>
#include <type_traits>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <vector>
#include <poll.h>

#define SYSCHECK(X) if ((X) == -1) { throw std::system_error(errno, std::system_category(), strerror(errno)); }

#define SOCK_NAME "@daemon-test.sock"
#define BUFF_SIZE 1024

class UNIXSocket {
    private:
        int fd;
        std::string msg;
        std::vector<char> buff;

        void initialize() {
            this->fd = socket(AF_UNIX, SOCK_STREAM, 0);
            SYSCHECK(this->fd)
        }

    public:
        UNIXSocket(UNIXSocket&&) = delete;
        UNIXSocket(const UNIXSocket&) = delete;
        UNIXSocket& operator=(UNIXSocket&&) = default;
        UNIXSocket& operator=(const UNIXSocket&) = delete;

        UNIXSocket(int fd = -1): buff(BUFF_SIZE) {
            if (fd == -1) {
                this->initialize();
            } else {
                this->fd = fd;
            }
        }

        ~UNIXSocket() {
            if (this->fd != -1) {
                close(this->fd);
            }
        }

        int& getFd() { return this->fd; }

        void Bind(std::string& path) {
            if (path.size() > 108 || path.size() == 0) {
                throw std::invalid_argument("invalid path");
            }

            struct sockaddr_un addr;
            std::memset(&addr, 0, sizeof(addr));
            addr.sun_family = AF_UNIX;
            strncpy(addr.sun_path, path.data(), path.size());
            addr.sun_path[0] = 0;

            {
                int ret = bind(
                    this->fd,
                    (sockaddr*)&addr,
                    sizeof(sa_family_t) + path.size()
                );
                SYSCHECK(ret)
            }
        }

        void Connect(std::string& path) {
            if (path.size() > 108 || path.size() == 0) {
                throw std::invalid_argument("invalid path");
            }

            struct sockaddr_un addr;
            std::memset(&addr, 0, sizeof(addr));
            addr.sun_family = AF_UNIX;
            strncpy(addr.sun_path, path.data(), path.size());
            addr.sun_path[0] = 0;

            {
                int ret = connect(
                    this->fd,
                    (sockaddr*)&addr,
                    sizeof(sa_family_t) + path.size()
                );
                SYSCHECK(ret)
            }
        }

        void Listen(int backlog) {
            int ret = listen(this->fd, backlog);
            SYSCHECK(ret)
        }

        int Accept() {
            int ret = accept(this->fd, NULL, NULL);
            SYSCHECK(ret)

            return ret;
        }

        std::string& Recv() {
            {
                int ret = recv(this->fd, this->buff.data(), this->buff.size(), 0);
                SYSCHECK(ret)
            }
            this->buff[this->buff.size() - 1] = 0;

            this->msg.clear();

            this->msg.insert(0, this->buff.data());

            std::fill(buff.begin(), buff.end(), 0);

            return this->msg;
        }

        void Send(std::string& msg) {
            if (msg.size() != 0) {
                int ret = send(this->fd, msg.data(), msg.size(), 0);
                SYSCHECK(ret)
            }
        }

        void Shutdown() {
            int ret = shutdown(this->fd, SHUT_RDWR);
            SYSCHECK(ret)
        }
};

class DServer {
    private:
        bool m_terminate = false;

        void start() {
            UNIXSocket server;

            try {
                std::string path = SOCK_NAME;
                server.Bind(path);
            } catch (const std::system_error& e) {
                if (e.code().value() == EADDRINUSE) {
                    std::cerr << e.code().message() << std::endl;
                    exit(EXIT_FAILURE);
                }

                throw e;
            }

            daemon(0, 0);

            server.Listen(10);

            std::string ret, msg;
            std::vector<std::unique_ptr<UNIXSocket>> clients;
            std::vector<struct pollfd> fds { { server.getFd(), POLLIN, 0} };

            while (!this->m_terminate) {
                int n = poll(fds.data(), fds.size(), 1);
                SYSCHECK(n)

                if (fds[0].revents & POLLIN) {
                    clients.push_back(std::make_unique<UNIXSocket>(server.Accept()));
                    fds.push_back({clients.back()->getFd(), POLLIN, 0});
                } else if (fds.size() > 1) {
                    for (auto it = fds.begin() + 1; it != fds.end();) {
                        const auto& client = std::find_if(clients.begin(), clients.end(),
                            [&it] (auto& ptr){
                                return ptr->getFd() == it->fd;
                            }
                        );

                        if (it->revents & POLLHUP) {
                            clients.erase(client);
                            it = fds.erase(it);
                            continue;
                        } else if (it->revents & POLLIN) {
                            msg = client->get()->Recv();

                            if (msg == "STOP") {
                                ret = "DServer stopped";
                                client->get()->Send(ret);

                                this->m_terminate = true;
                                continue;
                            }

                            ret = "DServer: " + msg;
                            client->get()->Send(ret);

                            ret.clear();
                        }

                        it++;
                    }
                }
            }
        }

    public:
        DServer() {
            this->start();
        }
};

class DClient {
    private:
        std::string msg;

        void start() {
            UNIXSocket server;
            try {
                std::string path = SOCK_NAME;
                server.Connect(path);
            } catch (const std::system_error& e) {
                if (e.code().value() == ECONNREFUSED) {
                    std::cerr << e.code().message() << std::endl;
                    exit(EXIT_FAILURE);
                }

                throw e;
            }

            server.Send(msg);
            msg = server.Recv();

            std::cout << msg << std::endl;
        }

    public:
        DClient(char *arg): msg(arg) {
            this->start();
        }
};

class Application {
    private:
        int argc;
        char **argv;

        void start() {
            if (argc == 2 && (!strcmp(argv[1], "-d") || !strcmp(argv[1], "--demonize"))) {
                this->startDaemon();
            } else if (argc == 3 && (!strcmp(argv[1], "-c") || !strcmp(argv[1], "--connect"))) {
                this->connectDaemon();
			} else if (argc == 2 && (!strcmp(argv[1], "-h") || !strcmp(argv[1], "--help"))) {
				this->printHelp();
            } else if (argc > 1) {
                this->printInvArg();
            } else {
				this->printHelp();
			}
        }

        void printHelp() {
            std::cout << "Usage " << argv[0] << " [OPTION]...\n"
                << "  -h, --help        display this help and exit\n"
                << "  -d, --demonize    demonize application\n"
                << "  -c, --connect=MSG connect to daemon" << std::endl;

			exit(EXIT_SUCCESS);
        }

		void printInvArg() {
			std::cerr << argv[0] << ": unrecognized option: " << argv[1] << "\n" 
				<< "Try '" << argv[0] << " --help' for more information" << std::endl;

			exit(EXIT_FAILURE);
		}

        void startDaemon() {
            DServer();
        }

        void connectDaemon() {
            DClient(this->argv[2]);
        }

    public:
        Application(int &argc, char **argv): argc(argc), argv(argv) {
            this->start();
        }
};

int main(int argc, char *argv[]) {
    Application app(argc, argv);

    return 0;
}

