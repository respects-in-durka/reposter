#include <td/telegram/Client.h>
#include <td/telegram/td_api.h>
#include <crow.h>

namespace detail {
    template<class... Fs>
    struct overload;

    template<class F>
    struct overload<F> : public F {
        explicit overload(F f) : F(f) {
        }
    };

    template<class F, class... Fs>
    struct overload<F, Fs...>
            : public overload<F>
              , public overload<Fs...> {
        explicit overload(F f, Fs... fs) : overload<F>(f), overload<Fs...>(fs...) {
        }

        using overload<F>::operator();
        using overload<Fs...>::operator();
    };
} // namespace detail

template<class... F>
auto overloaded(F... f) {
    return detail::overload<F...>(f...);
}

class TelegramAuthorizer {
public:
    TelegramAuthorizer(const std::int32_t api_id, const std::string &api_hash) {
        td::ClientManager::execute(td::td_api::make_object<td::td_api::setLogVerbosityLevel>(1));
        client_manager_ = std::make_unique<td::ClientManager>();
        client_id_ = client_manager_->create_client_id();
        api_id_ = api_id;
        api_hash_ = api_hash;
        send_query(td::td_api::make_object<td::td_api::getOption>("version"));
    }

    [[nodiscard]] bool is_authorized() const {
        while (!authorization_complete_) {
        }
        return authorized_;
    }

    void set_authorization_code(const std::string &code) {
        code_ = code;
        code_set_ = true;
    }

    void start_authorization(const std::string &phone) {
        phone_ = phone;
        std::thread authorizer_thread([this]() {
            while (!authorized_) {
                if (authorization_complete_) {
                    break;
                }

                auto [client_id, request_id, object] = client_manager_->receive(5);

                if (!object) {
                    continue;
                }

                downcast_call(*object, overloaded(
                                  [this](td::td_api::updateAuthorizationState &update_authorization_state) {
                                      downcast_call(*update_authorization_state.authorization_state_, overloaded(
                                                        [this](td::td_api::authorizationStateWaitTdlibParameters &) {
                                                            auto request = td::td_api::make_object<
                                                                td::td_api::setTdlibParameters>();
                                                            request->database_directory_ = "tdlib";
                                                            request->use_message_database_ = true;
                                                            request->use_secret_chats_ = true;
                                                            request->api_id_ = api_id_;
                                                            request->api_hash_ = api_hash_;
                                                            request->system_language_code_ = "en";
                                                            request->device_model_ = "Desktop";
                                                            request->application_version_ = "1.0";
                                                            send_query(std::move(request));
                                                        },
                                                        [this](td::td_api::authorizationStateWaitPhoneNumber &) {
                                                            send_query(
                                                                td::td_api::make_object<
                                                                    td::td_api::setAuthenticationPhoneNumber>(
                                                                    phone_, nullptr));
                                                        },
                                                        [this](td::td_api::authorizationStateWaitCode &) {
                                                            while (!code_set_) {
                                                            }

                                                            send_query(
                                                                td::td_api::make_object<
                                                                    td::td_api::checkAuthenticationCode>(code_));
                                                        },
                                                        [this](td::td_api::authorizationStateReady &) {
                                                            authorized_ = true;
                                                            authorization_complete_ = true;
                                                        },
                                                        [this](td::td_api::authorizationStateLoggingOut &) {
                                                            authorization_complete_ = true;
                                                        },
                                                        [this](td::td_api::authorizationStateClosed &) {
                                                            authorization_complete_ = true;
                                                        },
                                                        [](auto &) {
                                                        }
                                                    ));
                                  },
                                  [](auto &) {
                                  }));
            }
        });
        authorizer_thread.detach();
    }

private:
    using Object = td::td_api::object_ptr<td::td_api::Object>;
    std::unique_ptr<td::ClientManager> client_manager_;
    std::int32_t client_id_;
    std::uint64_t current_query_id_{0};
    bool authorized_{false};
    std::int32_t api_id_;
    std::string api_hash_;
    std::string phone_;
    bool code_set_{false};
    std::string code_;
    bool authorization_complete_{false};

    void send_query(td::td_api::object_ptr<td::td_api::Function> f) {
        const auto query_id = ++current_query_id_;
        client_manager_->send(client_id_, query_id, std::move(f));
    }
};

int main() {
    auto api_id = std::getenv("TD_API_ID");

    if (api_id == nullptr) {
        std::cerr << "API_ID missing" << std::endl;
        exit(1);
    }

    auto api_hash = std::getenv("TD_API_HASH");

    if (api_hash == nullptr) {
        std::cerr << "API_HASH missing" << std::endl;
        exit(1);
    }

    TelegramAuthorizer telegram_authorizer(*api_id, api_hash);
    crow::SimpleApp app;

    CROW_ROUTE(app, "/").methods("GET"_method)([] {
        const auto page = crow::mustache::load("phone.html");
        return page.render();
    });

    CROW_ROUTE(app, "/phone")([&telegram_authorizer](const crow::request &req, crow::response &res) {
        const std::string phone = req.url_params.get("phone");

        telegram_authorizer.start_authorization(phone);

        res.redirect("/code");
        res.end();
    });

    CROW_ROUTE(app, "/code")([] {
        const auto page = crow::mustache::load("code.html");
        return page.render();
    });

    CROW_ROUTE(app, "/confirm")([&telegram_authorizer](const crow::request &req, crow::response &res) {
        const auto code = req.url_params.get("code");

        telegram_authorizer.set_authorization_code(code);

        res.redirect("/status");
        res.end();
    });

    CROW_ROUTE(app, "/status")([&telegram_authorizer](crow::response &res) {
        if (!telegram_authorizer.is_authorized()) {
            res.end("Something went wrong");
            exit(1);
        }
        res.end("Success");
        exit(0);
    });

    app.port(3333).run();
}
