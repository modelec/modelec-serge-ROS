#include <modelec/pcb_alim_interface.hpp>
#include <modelec/utils.hpp>
#include <modelec_interface/srv/add_serial_listener.hpp>

namespace Modelec
{
    PCBAlimInterface::PCBAlimInterface() : Node("pcb_alim_interface")
    {
        // Service to create a new serial listener
        auto request = std::make_shared<modelec_interface::srv::AddSerialListener::Request>();
        request->name = "pcb_alim";
        request->bauds = 115200;
        request->serial_port = "/dev/serial0"; // TODO : check the real serial port
        auto client = this->create_client<modelec_interface::srv::AddSerialListener>("add_serial_listener");
        while (!client->wait_for_service(std::chrono::seconds(1)))
        {
            if (!rclcpp::ok())
            {
                RCLCPP_ERROR(this->get_logger(), "Interrupted while waiting for the service. Exiting.");
                return;
            }
            RCLCPP_INFO(this->get_logger(), "service not available, waiting again...");
        }
        auto result = client->async_send_request(request);
        if (rclcpp::spin_until_future_complete(this->get_node_base_interface(), result) ==
            rclcpp::FutureReturnCode::SUCCESS)
        {
            if (auto res = result.get())
            {
                if (res->success)
                {
                    RCLCPP_INFO(this->get_logger(), "Publisher: %s", res->publisher.c_str());
                    RCLCPP_INFO(this->get_logger(), "Subscriber: %s", res->subscriber.c_str());

                    pcb_callback_group_ = this->create_callback_group(rclcpp::CallbackGroupType::MutuallyExclusive);

                    rclcpp::SubscriptionOptions options;
                    options.callback_group = pcb_callback_group_;

                    pcb_subscriber_ = this->create_subscription<std_msgs::msg::String>(
                        result.get()->publisher, 10,
                        [this](const std_msgs::msg::String::SharedPtr msg)
                        {
                            PCBCallback(msg);
                        }, options);

                    pcb_executor_ = std::make_shared<rclcpp::executors::SingleThreadedExecutor>();
                    pcb_executor_->add_callback_group(pcb_callback_group_, this->get_node_base_interface());

                    pcb_executor_thread_ = std::thread([this]()
                    {
                        pcb_executor_->spin();
                    });

                    pcb_publisher_ = this->create_publisher<std_msgs::msg::String>(result.get()->subscriber, 10);
                }
                else
                {
                    RCLCPP_ERROR(this->get_logger(), "Failed to add serial listener");
                }
            }
            else
            {
                RCLCPP_ERROR(this->get_logger(), "Failed to ask for a serial listener");
            }
        }
        else
        {
            RCLCPP_ERROR(this->get_logger(), "Service call failed");
        }

        pcb_out_service_ = create_service<modelec_interface::srv::AlimOut>(
            "alim/out",
            [this](const std::shared_ptr<modelec_interface::srv::AlimOut::Request> request,
                   std::shared_ptr<modelec_interface::srv::AlimOut::Response> response)
            {
                if (request->enable != -1 && request->type == modelec_interface::srv::AlimOut::Request::STATE)
                {
                    HandleSetPCBOutData(request, response);
                }
                else
                {
                    HandleGetPCBOutData(request, response);
                }
            });

        pcb_in_service_ = create_service<modelec_interface::srv::AlimIn>(
            "alim/in",
            [this](const std::shared_ptr<modelec_interface::srv::AlimIn::Request> request,
                   std::shared_ptr<modelec_interface::srv::AlimIn::Response> response)
            {
                HandleGetPCBInData(request, response);
            });

        pcb_bau_service_ = create_service<modelec_interface::srv::AlimBau>(
            "alim/bau",
            [this](const std::shared_ptr<modelec_interface::srv::AlimBau::Request> request,
                   std::shared_ptr<modelec_interface::srv::AlimBau::Response> response)
            {
                HandleGetPCBBauData(request, response);
            });

        pcb_emg_service_ = create_service<modelec_interface::srv::AlimEmg>(
            "alim/emg",
            [this](const std::shared_ptr<modelec_interface::srv::AlimEmg::Request> request,
                   std::shared_ptr<modelec_interface::srv::AlimEmg::Response> response)
            {
                HandleSetPCBEmgData(request, response);
            });

        pcb_temp_service_ = create_service<modelec_interface::srv::AlimTemp>(
            "alim/temp",
            [this](const std::shared_ptr<modelec_interface::srv::AlimTemp::Request> request,
                   std::shared_ptr<modelec_interface::srv::AlimTemp::Response> response)
            {
                HandleGetPCBTempData(request, response);
            });
    }

    PCBAlimInterface::~PCBAlimInterface()
    {
        pcb_executor_->cancel();
        if (pcb_executor_thread_.joinable())
        {
            pcb_executor_thread_.join();
        }
    }

    void PCBAlimInterface::PCBCallback(const std_msgs::msg::String::SharedPtr msg)
    {
        RCLCPP_DEBUG(this->get_logger(), "Received message: '%s'", msg->data.c_str());
        std::vector<std::string> tokens = split(msg->data, ';');
        if (tokens.size() < 2)
        {
            RCLCPP_ERROR(this->get_logger(), "Invalid message format: %s", msg->data.c_str());
            return;
        }

        if (tokens[0] == "SET")
        {
            if (startsWith(tokens[1], "OUT"))
            {
                bool success = true;
                int value = std::stoi(tokens[3]);
                ResolveGetPCBOutRequest({success, value});
            }
            else if (startsWith(tokens[1], "IN"))
            {
                bool success = true;
                int value = std::stoi(tokens[3]);
                ResolveGetPCBInRequest({success, value});
            }
            else if (tokens[1] == "BAU")
            {
                bool success = true;
                bool activate = (tokens[3] == "1");
                ResolveGetPCBBauRequest({success, activate});
            }
            else if (tokens[1] == "TEMP")
            {
                bool success = true;
                int value = std::stoi(tokens[3]);
                ResolveGetPCBTempRequest({success, value});
            }
        }
        else if (tokens[0] == "OK")
        {
            if (startsWith(tokens[1], "OUT"))
            {
                bool success = true;
                int value = std::stoi(tokens[3]);

                ResolveSetPCBOutRequest({success, value});
            }
            else if (tokens[1] == "EMG")
            {
                bool success = true;
                bool value = (tokens[3] == "1");
                ResolveSetPCBEmgRequest({success, value});
            }
        }
        else if (tokens[0] == "KO")
        {
            if (startsWith(tokens[1], "OUT"))
            {
                bool success = false;
                int value = -1;

                ResolveSetPCBOutRequest({success, value});
            }
            else if (tokens[1] == "EMG")
            {
                bool success = false;
                int value = -1;

                ResolveSetPCBEmgRequest({success, value});
            }
        }
        else
        {
            RCLCPP_ERROR(this->get_logger(), "Unknown message type: %s", tokens[0].c_str());
        }
    }

    void PCBAlimInterface::HandleGetPCBOutData(const std::shared_ptr<modelec_interface::srv::AlimOut::Request> request,
                                               std::shared_ptr<modelec_interface::srv::AlimOut::Response> response)
    {
        std::promise<PCBData> promise;
        auto future = promise.get_future();

        {
            std::lock_guard<std::mutex> lock(pcb_out_mutex_);
            pcb_out_promises_.push(std::move(promise));
        }

        GetData(request->out, {request->type});

        PCBData result = future.get();

        response->success = result.success;
        response->result = result.value;
    }

    void PCBAlimInterface::HandleSetPCBOutData(const std::shared_ptr<modelec_interface::srv::AlimOut::Request> request,
                                               std::shared_ptr<modelec_interface::srv::AlimOut::Response> response)
    {
        std::promise<PCBData> promise;
        auto future = promise.get_future();

        {
            std::lock_guard<std::mutex> lock(pcb_out_mutex_);
            pcb_out_promises_.push(std::move(promise));
        }

        SendOrder(request->out, {request->type, request->enable == true ? "1" : "0"});

        PCBData result = future.get();

        response->success = result.success;
        response->result = result.value;
    }

    void PCBAlimInterface::HandleGetPCBInData(const std::shared_ptr<modelec_interface::srv::AlimIn::Request> request,
                                              std::shared_ptr<modelec_interface::srv::AlimIn::Response> response)
    {
        std::promise<PCBData> promise;
        auto future = promise.get_future();

        {
            std::lock_guard<std::mutex> lock(pcb_out_mutex_);
            pcb_out_promises_.push(std::move(promise));
        }

        GetData("IN" + request->input, {request->type});

        PCBData result = future.get();

        response->success = result.success;
        response->result = result.value;
    }

    void PCBAlimInterface::HandleGetPCBBauData(const std::shared_ptr<modelec_interface::srv::AlimBau::Request> request,
                                               std::shared_ptr<modelec_interface::srv::AlimBau::Response> response)
    {
        std::promise<PCBBau> promise;
        auto future = promise.get_future();

        {
            std::lock_guard<std::mutex> lock(pcb_bau_mutex_);
            pcb_bau_promises_.push(std::move(promise));
        }

        GetData("BAU", {"STATE"});

        PCBBau result = future.get();
        response->success = result.success;
        response->activate = result.activate;
    }

    void PCBAlimInterface::HandleSetPCBEmgData(const std::shared_ptr<modelec_interface::srv::AlimEmg::Request> request,
                                               std::shared_ptr<modelec_interface::srv::AlimEmg::Response> response)
    {
        std::promise<bool> promise;
        auto future = promise.get_future();

        {
            std::lock_guard<std::mutex> lock(pcb_emg_mutex_);
            pcb_emg_promises_.push(std::move(promise));
        }

        SendOrder("EMG", {"STATE", request->activate == true ? "1" : "0"});

        response->success = future.get();
    }

    void PCBAlimInterface::HandleGetPCBTempData(
        const std::shared_ptr<modelec_interface::srv::AlimTemp::Request> request,
        std::shared_ptr<modelec_interface::srv::AlimTemp::Response> response)
    {
        std::promise<PCBData> promise;
        auto future = promise.get_future();

        {
            std::lock_guard<std::mutex> lock(pcb_temp_mutex_);
            pcb_temp_promises_.push(std::move(promise));
        }

        SendOrder("TEMP", {"CELS"});

        PCBData result = future.get();
        response->success = result.success;
        response->value = result.value;
    }

    void PCBAlimInterface::ResolveGetPCBOutRequest(const PCBData& value)
    {
        std::lock_guard<std::mutex> lock(pcb_out_mutex_);
        if (!pcb_out_promises_.empty())
        {
            auto promise = std::move(pcb_out_promises_.front());
            pcb_out_promises_.pop();
            promise.set_value(value);
        }
        else
        {
            RCLCPP_DEBUG(this->get_logger(), "No pending request for PCB out data");
        }
    }

    void PCBAlimInterface::ResolveSetPCBOutRequest(const PCBData& value)
    {
        std::lock_guard<std::mutex> lock(pcb_out_mutex_);
        if (!pcb_out_promises_.empty())
        {
            auto promise = std::move(pcb_out_promises_.front());
            pcb_out_promises_.pop();
            promise.set_value(value);
        }
        else
        {
            RCLCPP_DEBUG(this->get_logger(), "No pending request for PCB out data");
        }
    }

    void PCBAlimInterface::ResolveGetPCBInRequest(const PCBData& value)
    {
        std::lock_guard<std::mutex> lock(pcb_in_mutex_);
        if (!pcb_in_promises_.empty())
        {
            auto promise = std::move(pcb_in_promises_.front());
            pcb_in_promises_.pop();
            promise.set_value(value);
        }
        else
        {
            RCLCPP_DEBUG(this->get_logger(), "No pending request for PCB in data");
        }
    }

    void PCBAlimInterface::SendToPCB(const std::string& data) const
    {
        auto message = std_msgs::msg::String();
        message.data = data;
        pcb_publisher_->publish(message);
    }

    void PCBAlimInterface::SendToPCB(const std::string& order, const std::string& elem,
                                    const std::vector<std::string>& data) const
    {
        std::string command = order + ";" + elem;
        for (const auto& d : data)
        {
            command += ";" + d;
        }

        SendToPCB(command);
    }

    void PCBAlimInterface::GetData(const std::string& elem, const std::vector<std::string>& data) const
    {
        SendToPCB("GET", elem, data);
    }

    void PCBAlimInterface::SendOrder(const std::string& elem, const std::vector<std::string>& data) const
    {
        SendToPCB("SET", elem, data);
    }
} // namespace Modelec

int main(int argc, char** argv)
{
    rclcpp::init(argc, argv);
    auto node = std::make_shared<Modelec::PCBAlimInterface>();

    // Increase number of threads explicitly!
    rclcpp::executors::MultiThreadedExecutor executor(
        rclcpp::ExecutorOptions(), 2 /* or more threads! */);

    executor.add_node(node);
    executor.spin();
    rclcpp::shutdown();
    return 0;
}
