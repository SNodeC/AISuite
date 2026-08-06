#define AISUITE_DECLARE_CODEX_FRONTEND_CLIENT_FACADE(Name)                                                                                 \
    class Name {                                                                                                                           \
    public:                                                                                                                                \
        Name(const Name&) = delete;                                                                                                        \
        Name(Name&&) = delete;                                                                                                             \
        Name& operator=(const Name&) = delete;                                                                                             \
        Name& operator=(Name&&) = delete;                                                                                                  \
                                                                                                                                           \
    private:                                                                                                                               \
        friend class Client;                                                                                                               \
        explicit Name(Client& owner) noexcept                                                                                              \
            : client(&owner) {                                                                                                             \
        }                                                                                                                                  \
        Client* client;                                                                                                                    \
    }
