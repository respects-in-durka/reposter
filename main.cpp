#include <iostream>

#include <td/telegram/Client.h>
#include <td/telegram/td_api.h>
#include <cstdint>
#include <functional>
#include <map>
#include <sstream>
#include <string>
#include <format>

#include <amqpcpp.h>
#include <amqpcpp/libboostasio.h>
#include <filesystem>

#include <boost/asio/io_service.hpp>
#include <boost/asio/deadline_timer.hpp>
#include <boost/json/src.hpp>
#include <boost/algorithm/string/split.hpp>
#include <boost/algorithm/string/classification.hpp>

#define CROW_STATIC_DIRECTORY "tdlib/static"
#define CROW_STATIC_ENDPOINT "/tdlib/static/<path>"
#include <crow.h>

namespace detail {
    template<class... Fs>
    struct overload;

    template<class F>
    struct overload<F> : F {
        explicit overload(F f) : F(f) {
        }
    };

    template<class F, class... Fs>
    struct overload<F, Fs...>
            : overload<F>
              , overload<Fs...> {
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

class TelegramClient {
public:
    TelegramClient(const std::int32_t api_id, const std::string &api_hash, const std::string &channels_string,
                   const std::string &base_url, const std::string &rabbit_queue, AMQP::TcpChannel *rabbit_channel) {
        td::ClientManager::execute(td::td_api::make_object<td::td_api::setLogVerbosityLevel>(1));
        client_manager_ = std::make_unique<td::ClientManager>();
        client_id_ = client_manager_->create_client_id();

        api_id_ = api_id;
        api_hash_ = api_hash;
        base_url_ = base_url;
        rabbit_channel_ = rabbit_channel;
        rabbit_queue_ = rabbit_queue;

        std::vector<std::string> channels;
        split(channels, channels_string, boost::is_any_of(", "), boost::token_compress_on);

        for (auto &channel : channels) {
            chat_ids_.push_back(std::stol(channel));
        }

        send_query(td::td_api::make_object<td::td_api::getOption>("version"), {});
        authorize();
    }

    void start() {
        std::cout << "Starting telegram client..." << std::endl;
        while (true) {
            auto response = client_manager_->receive(3);
            if (!response.object) {
                continue;
            }

            process_response(std::move(response));
        }
    }

    void process_update(td::td_api::object_ptr<td::td_api::Object> update) {
        downcast_call(
            *update, overloaded(
                [this](td::td_api::updateNewChat &update_new_chat) {
                    if (std::ranges::find(chat_ids_, update_new_chat.chat_->id_) == chat_ids_.end()) {
                        return;
                    }

                    chat_title_[update_new_chat.chat_->id_] = update_new_chat.chat_->title_;


                    // TODO service must be publicly available for this to work
                    if (update_new_chat.chat_->photo_ != nullptr) {
                        auto download_request = td::td_api::make_object<td::td_api::downloadFile>();
                        download_request->file_id_ = update_new_chat.chat_->photo_->big_->id_;
                        download_request->priority_ = 32;
                        download_request->offset_ = 0;
                        download_request->limit_ = 0;
                        download_request->synchronous_ = true;

                        auto new_chat = std::move(update_new_chat.chat_);

                        send_query(std::move(download_request),
                                   [this, &new_chat](Object object) {
                                       if (object->get_id() == td::td_api::error::ID) {
                                           return;
                                       }
                                       auto file = td::move_tl_object_as<td::td_api::file>(object);

                                       auto cwd = std::filesystem::current_path();
                                       auto relative = std::filesystem::relative(file->local_->path_, cwd);
                                       chat_icons_[new_chat->id_] = std::format("{}/{}", base_url_, relative.c_str());
                                   });
                    } else {
                        chat_icons_[update_new_chat.chat_->id_] =
                                "https://seeklogo.com/images/T/telegram-logo-2A32756393-seeklogo.com.png";
                    }
                },
                [this](const td::td_api::updateChatTitle &update_chat_title) {
                    if (std::ranges::find(chat_ids_, update_chat_title.chat_id_) == chat_ids_.end()) {
                        return;
                    }
                    chat_title_[update_chat_title.chat_id_] = update_chat_title.title_;
                },
                [this](td::td_api::updateNewMessage &update_new_message) {
                    auto message = std::move(update_new_message.message_);

                    if (std::ranges::find(chat_ids_, message->chat_id_) == chat_ids_.end()) {
                        return;
                    }

                    boost::json::object rabbit_message;

                    rabbit_message["chat_name"] = chat_title_[message->chat_id_];
                    rabbit_message["chat_icon"] =
                            "https://seeklogo.com/images/T/telegram-logo-2A32756393-seeklogo.com.png";

                    downcast_call(*message->content_, overloaded(
                                      [this, &rabbit_message](td::td_api::messageAnimation &animation_message) {
                                          rabbit_message["text"] = animation_message.caption_->text_;

                                          std::string mimetype = animation_message.animation_->mime_type_;
                                          std::string extension = mimetype.substr(mimetype.find('/') + 1);
                                          std::string animation_path = std::format(
                                              "tdlib/static/{}.{}",
                                              animation_message.animation_->animation_->remote_->unique_id_,
                                              extension
                                          );

                                          auto animation_url = std::format("{}/{}", base_url_, animation_path);
                                          rabbit_message["files"] = {animation_url};

                                          auto download_request = td::td_api::make_object<td::td_api::downloadFile>();
                                          download_request->file_id_ = animation_message.animation_->animation_->id_;
                                          download_request->priority_ = 32;
                                          download_request->offset_ = 0;
                                          download_request->limit_ = 0;
                                          download_request->synchronous_ = true;

                                          send_query(std::move(download_request),
                                                     [this, animation_path](Object object) {
                                                         if (object->get_id() == td::td_api::error::ID) {
                                                             return;
                                                         }
                                                         auto file = td::move_tl_object_as<td::td_api::file>(object);

                                                         std::filesystem::rename(file->local_->path_, animation_path);
                                                     });
                                      },
                                      [this, &rabbit_message](td::td_api::messageSticker &sticker_message) {
                                          if (sticker_message.sticker_->format_->get_id() ==
                                              td::td_api::stickerFormatTgs::ID) {
                                              return;
                                          }

                                          std::string sticker_path;
                                          downcast_call(*sticker_message.sticker_->format_, overloaded(
                                                            [this, &sticker_path, &sticker_message](
                                                        td::td_api::stickerFormatWebm &) {
                                                                sticker_path = std::format(
                                                                    "tdlib/static/{}.webm",
                                                                    sticker_message.sticker_->sticker_->remote_->id_);
                                                            },
                                                            [this, &sticker_path, &sticker_message](
                                                        td::td_api::stickerFormatWebp &) {
                                                                sticker_path = std::format(
                                                                    "tdlib/static/{}.webp",
                                                                    sticker_message.sticker_->sticker_->remote_->id_);
                                                            },
                                                            [](auto &) {
                                                            }
                                                        ));
                                          auto sticker_url = std::format("{}/{}", base_url_, sticker_path);
                                          rabbit_message["files"] = {sticker_url};

                                          auto download_request = td::td_api::make_object<td::td_api::downloadFile>();
                                          download_request->file_id_ = sticker_message.sticker_->sticker_->id_;
                                          download_request->priority_ = 32;
                                          download_request->offset_ = 0;
                                          download_request->limit_ = 0;
                                          download_request->synchronous_ = true;

                                          send_query(std::move(download_request),
                                                     [this, sticker_path](Object object) {
                                                         if (object->get_id() == td::td_api::error::ID) {
                                                             return;
                                                         }
                                                         auto file = td::move_tl_object_as<td::td_api::file>(object);

                                                         std::filesystem::rename(file->local_->path_, sticker_path);
                                                     });
                                      },
                                      [this, &rabbit_message](td::td_api::messageText &text_message) {
                                          rabbit_message["text"] = text_message.text_->text_;
                                      },
                                      [this, &rabbit_message](td::td_api::messagePhoto &photo_message) {
                                          rabbit_message["text"] = photo_message.caption_->text_;

                                          size_t photo_index = photo_message.photo_->sizes_.size() - 1;
                                          auto photo = std::move(photo_message.photo_->sizes_[photo_index]->photo_);

                                          std::string photo_path = std::format(
                                              "tdlib/static/{}.jpg",
                                              photo->remote_->unique_id_
                                          );
                                          std::string photo_url = std::format("{}/{}", base_url_, photo_path);
                                          rabbit_message["files"] = {photo_url};

                                          auto download_request = td::td_api::make_object<td::td_api::downloadFile>();
                                          download_request->file_id_ = photo->id_;
                                          download_request->priority_ = 32;
                                          download_request->offset_ = 0;
                                          download_request->limit_ = 0;
                                          download_request->synchronous_ = true;

                                          send_query(std::move(download_request),
                                                     [this, photo_path](Object object) {
                                                         if (object->get_id() == td::td_api::error::ID) {
                                                             return;
                                                         }
                                                         auto file = td::move_tl_object_as<td::td_api::file>(object);

                                                         std::filesystem::rename(file->local_->path_, photo_path);
                                                     });
                                      },
                                      [this, &rabbit_message](td::td_api::messageVideo &video_message) {
                                          rabbit_message["text"] = video_message.caption_->text_;

                                          std::string mimetype = video_message.video_->mime_type_;
                                          std::string extension = mimetype.substr(mimetype.find('/') + 1);
                                          std::string video_path = std::format(
                                              "tdlib/static/{}.{}",
                                              video_message.video_->video_->remote_->unique_id_,
                                              extension
                                          );

                                          auto video_url = std::format("{}/{}", base_url_, video_path);
                                          rabbit_message["files"] = {video_url};

                                          auto download_request = td::td_api::make_object<td::td_api::downloadFile>();
                                          download_request->file_id_ = video_message.video_->video_->id_;
                                          download_request->priority_ = 32;
                                          download_request->offset_ = 0;
                                          download_request->limit_ = 0;
                                          download_request->synchronous_ = true;

                                          send_query(std::move(download_request),
                                                     [this, video_path](Object object) {
                                                         if (object->get_id() == td::td_api::error::ID) {
                                                             return;
                                                         }
                                                         auto file = td::move_tl_object_as<td::td_api::file>(object);

                                                         std::filesystem::rename(file->local_->path_, video_path);
                                                     });
                                      },
                                      [this, &rabbit_message](td::td_api::messageVideoNote &video_note_message) {
                                          std::string video_path = std::format(
                                              "tdlib/static/{}.jpg",
                                              video_note_message.video_note_->video_->remote_->unique_id_
                                          );
                                          std::string video_url = std::format("{}/{}", base_url_, video_path);
                                          rabbit_message["files"] = {video_url};


                                          auto download_request = td::td_api::make_object<td::td_api::downloadFile>();
                                          download_request->file_id_ = video_note_message.video_note_->video_->id_;
                                          download_request->priority_ = 32;
                                          download_request->offset_ = 0;
                                          download_request->limit_ = 0;
                                          download_request->synchronous_ = true;

                                          send_query(std::move(download_request),
                                                     [this, video_path](Object object) {
                                                         if (object->get_id() == td::td_api::error::ID) {
                                                             return;
                                                         }
                                                         auto file = td::move_tl_object_as<td::td_api::file>(object);

                                                         std::filesystem::rename(file->local_->path_, video_path);
                                                     });
                                      },
                                      [](auto &) {
                                      }
                                  ));

                    rabbit_channel_->publish("", rabbit_queue_, serialize(rabbit_message));
                },
                [](auto &) {
                }));
    }

    void process_response(td::ClientManager::Response response) {
        if (!response.object) {
            return;
        }

        if (response.request_id == 0) {
            return process_update(std::move(response.object));
        }


        auto it = handlers_.find(response.request_id);
        if (it != handlers_.end()) {
            it->second(std::move(response.object));
            handlers_.erase(it);
        }
    }

private:
    using Object = td::td_api::object_ptr<td::td_api::Object>;

    AMQP::TcpChannel *rabbit_channel_;
    std::string rabbit_queue_;
    std::unique_ptr<td::ClientManager> client_manager_;
    std::int32_t client_id_;
    bool authorized_{false};
    std::int32_t api_id_;
    std::string api_hash_;
    std::string base_url_;

    std::vector<int64_t> chat_ids_;
    std::map<std::int64_t, std::string> chat_title_{};
    std::map<std::int64_t, std::string> chat_icons_{};

    std::int32_t query_id_{0};
    std::map<std::uint64_t, std::function<void(td::td_api::object_ptr<td::td_api::Object>)> > handlers_;

    void send_query(td::td_api::object_ptr<td::td_api::Function> f,
                    std::function<void(td::td_api::object_ptr<td::td_api::Object>)> handler) {
        auto query_id = ++query_id_;
        if (handler) {
            handlers_.emplace(query_id, std::move(handler));
        }
        client_manager_->send(client_id_, query_id, std::move(f));
    }

    void authorize() {
        // ReSharper disable once CppDFAConstantConditions
        while (!authorized_) {
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
                                                        send_query(std::move(request), {});
                                                    },
                                                    [this](td::td_api::authorizationStateReady &) {
                                                        authorized_ = true;
                                                    },
                                                    [](auto &) {
                                                    }
                                                ));
                              },
                              [](auto &) {
                              }));
        }
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

    auto base_url = std::getenv("TD_BASE_URL");

    if (base_url == nullptr) {
        std::cerr << "BASE_URL missing" << std::endl;
        exit(1);
    }
    auto chats = std::getenv("TD_CHATS");

    if (chats == nullptr) {
        std::cerr << "CHATS missing" << std::endl;
        exit(1);
    }

    #ifdef DEBUG
    crow::SimpleApp app;
    std::thread crow_thread([&app]() {
        app.port(3334).run();
    });
    crow_thread.detach();
    #endif

    auto rabbit_url = std::getenv("TD_RABBIT_URL");

    if (rabbit_url == nullptr) {
        std::cerr << "RABBIT_URL missing" << std::endl;
        exit(1);
    }

    auto rabbit_queue = std::getenv("TD_RABBIT_QUEUE");

    if (rabbit_queue == nullptr) {
        std::cerr << "RABBIT_QUEUE missing" << std::endl;
        exit(1);
    }

    boost::asio::io_service service(4);
    AMQP::LibBoostAsioHandler handler(service);
    AMQP::TcpConnection connection(&handler, AMQP::Address(rabbit_url));
    AMQP::TcpChannel channel(&connection);

    channel.declareQueue(rabbit_queue).onSuccess(
        [](const std::string &name, uint32_t messages, uint32_t consumers) {
            std::cout << "declared queue " << name << std::endl;
        });

    std::thread rabbit_thread([&service]() {
        service.run();
    });
    rabbit_thread.detach();

    TelegramClient client(*api_id, api_hash, chats, base_url, rabbit_queue, &channel);
    client.start();
}
