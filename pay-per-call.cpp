#include <iostream>
#include <data/io/arg_parser.hpp>
#include <data/net/TCP.hpp>

#include <Cosmos/context.hpp>

#include <data/io/exception.hpp>
#include <data/net/URL.hpp>
#include <data/net/HTTP_server.hpp>

using namespace data;

struct program_input : Cosmos::context {
    program_input (const io::arg_parser &);

    net::IP::TCP::endpoint Endpoint;
};

Cosmos::error run_program (program_input &);

int main (int arg_count, char **arg_values) {
    program_input p {io::arg_parser (arg_count, arg_values)};
    Cosmos::error e = run_program (p);
    if (e.Code != 0) {
        if (e.Message) std::cout << "Program failed with error: " << *e.Message << std::endl;
        else std::cout << "Program failed with error code " << e.Code << std::endl;
    }
    return e.Code;
}

Cosmos::error run_program (program_input &input) {

    boost::asio::io_context ioc;
    Cosmos::watch_wallet &w = *input.watch_wallet ();

    net::HTTP::server server {ioc, input.Endpoint, [w] (const net::HTTP::request &req) -> net::HTTP::response {
        // is this a payment request for an new address?

        // otherwise, check if a payment was included.

        // if not send a 402 error.

        // otherwise validate the payment.

        // forward the API call.

        return net::HTTP::response {net::HTTP::status {404}, {}, ""};
    }};

    try {
        ioc.run ();
    } catch (const data::exception &x) {
        return Cosmos::error {x.Code, string {x.what ()}};
    } catch (const std::exception &x) {
        return Cosmos::error {string {x.what ()}};
    } catch (int x) {
        return Cosmos::error {x};
    } catch (...) {
        return Cosmos::error {"Unknown error encountered."};
    }

    return Cosmos::error {};
}



