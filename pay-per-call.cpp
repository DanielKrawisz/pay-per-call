#include <iostream>
#include <data/io/arg_parser.hpp>
#include <Cosmos/context.hpp>

using namespace data;

Cosmos::error run_program (const io::arg_parser &);

// run the program and print any errors that were returned.
int main (int arg_count, char **arg_values) {
    Cosmos::error e = run_program (io::arg_parser (arg_count, arg_values));

    if (e.Code != 0) {
        if (e.Message) std::cout << "Program failed with error: " << *e.Message << std::endl;
        else std::cout << "Program failed with error code " << e.Code << std::endl;
    }

    return e.Code;
}

#include <data/net/HTTP_server.hpp>
#include <data/net/asio/periodic_timer.hpp>
#include <gigamonkey/fees.hpp>

namespace Bitcoin = Gigamonkey::Bitcoin;
namespace asio = data::net::asio;
using satoshi_per_byte = Gigamonkey::satoshi_per_byte;
using digest160 = Gigamonkey::digest160;
using duration = std::chrono::duration<int, std::milli>;
using time_point = std::chrono::time_point<std::chrono::system_clock>;

// program parameters that will be read in from the command line.
struct parameters {

    // endpoint we will listen from.
    net::IP::TCP::endpoint Endpoint;

    // an address expires after 10 minutes of not being in use.
    duration InactiveAddressExpiration {600000};

    // how long will we accept payments to an address before we expire it.
    duration AbsoluteAddressExpiration {3600000};

    // expected network fees.
    satoshi_per_byte ExpectedFee {50, 1000};

    uint32 MaxExpectedOutputsPerPayment {10};

    parameters (const io::arg_parser &);
};

class pay_per_call {

    // Cosmos::context is the wallet and the connection to the Bitcoin network.
    // not necessarily a great interface ATM but it works ok.
    Cosmos::context &Wallet;

    // we will keep track of addresses we have provided to users.
    struct payment_address {
        // how to find the key that redeems this address.
        Cosmos::derivation Derivation;
        // when was the address derived.
        time_point DerivationTime;
        // when was the address last seen.
        time_point LastSeen;
    };

    // set of active addresses.
    std::map<digest160, payment_address> Addresses;

    // this represents a single call that we support as a paid service.
    struct call {
        // the price to be paid to make the call.
        Bitcoin::satoshi Price;
        // make the call
        virtual net::HTTP::response operator () () = 0;
        virtual ~call () {}
    };

    struct calls {
        // check a url to see if we offer it as a service.
        // if not, nullptr will be returned. Otherwise, we
        // will get a price and a function to call if the
        // payment is valid.
        ptr<call> operator () (const net::URL &u);

        calls ();
    };

    calls Calls;

    // a timer to remove inactive addresses.
    ptr<net::asio::periodic_timer> Timer;

    // make a new 402 response to send back to a user for a given price.
    net::HTTP::response make_402 (Bitcoin::satoshi);

public:
    parameters Parameter;

    pay_per_call (asio::io_context &ioc, Cosmos::context &, const arg_parser &p);
    net::HTTP::response operator () (const net::HTTP::request &req);
};

#include <data/io/exception.hpp>
#include <data/net/HTTP_server.hpp>

// catch all exceptions and return the result.
// right now we don't have a way to end the program other than ctrl^C
// note that the wallets don't get saved if the program fails to close normally,
// so this version of the program as it is now is pretty much inadequate.
Cosmos::error run_program (const arg_parser &p) {

    try {

        // load wallet.
        Cosmos::context Wallet {};
        Cosmos::read_watch_wallet_options (Wallet, p);

        // when we run this, all async calls will begin operation.
        boost::asio::io_context ioc;

        pay_per_call program {ioc, Wallet, p};
        net::HTTP::server server {ioc, program.Parameter.Endpoint, program};

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

#include <gigamonkey/script/pattern/pay_to_address.hpp>

using pay_to_address = Gigamonkey::pay_to_address;

pay_per_call::pay_per_call (asio::io_context &ioc, Cosmos::context &wallet, const arg_parser &p):
    Wallet {wallet}, Addresses {}, Calls {},
    Timer {new net::asio::periodic_timer {ioc,
        // run every 10 seconds.
        boost::posix_time::time_duration {0, 0, 10, 0},
        [this] () {
            // go through all the addresses and remove expired.
            time_point now = std::chrono::system_clock::now ();
            for (auto it = this->Addresses.cbegin (); it != this->Addresses.cend ();)
                if (now - it->second.LastSeen < this->Parameter.InactiveAddressExpiration ||
                    now - it->second.DerivationTime < this->Parameter.AbsoluteAddressExpiration
                ) it = this->Addresses.erase (it);
                else ++it;
        }
    }}, Parameter {p} {}

Bitcoin::satoshi inline round_up (double sats) {
    return Bitcoin::satoshi {int64 (sats + .5)};
}

// add enough extra in case the user wants to send a payment in up to expected_outputs outputs for some reason.
Bitcoin::satoshi inline demanded_price (Bitcoin::satoshi base_price, satoshi_per_byte expected_fee, uint32 expected_outputs) {
    return base_price + round_up (10 * double (expected_fee) * pay_to_address::redeem_expected_size ());
}

net::HTTP::response pay_per_call::operator () (const net::HTTP::request &req) {
    // do we recognize this call?
    ptr<call> offer = Calls (req.URL);
    if (offer == nullptr) return net::HTTP::response {net::HTTP::status {404}, {}, ""};

    // has a payment been included?
    Cosmos::processed processed {req.Body};
    Bitcoin::transaction tx {processed.Transaction};

    if (!tx.valid ()) return make_402 (demanded_price (offer->Price, Parameter.ExpectedFee, Parameter.MaxExpectedOutputsPerPayment));

    Bitcoin::txid id = processed.id ();

    // do we recognize the address?
    Bitcoin::satoshi value_paid_to_us {0};
    list<entry<Bitcoin::outpoint, Cosmos::redeemable>> our_new_outputs {};
    uint32 index = 0;
    for (const Bitcoin::output &out : tx.Outputs) {
        pay_to_address script {out.Script};
        if (script.valid ()) {
            auto it = Addresses.find (script.Address);
            if (it != Addresses.end ()) {
                value_paid_to_us += out.Value;
                our_new_outputs <<= entry {Bitcoin::outpoint {id, index},
                    Cosmos::redeemable {out, it->second.Derivation, pay_to_address::redeem_expected_size ()}};
            }
        }
        index++;
    }

    // is the payment enough for the price?
    if (value_paid_to_us - round_up (
        double (Parameter.ExpectedFee) *
        data::size (our_new_outputs) *
        pay_to_address::redeem_expected_size ()) < offer->Price)
        return make_402 (demanded_price (offer->Price, Parameter.ExpectedFee, Parameter.MaxExpectedOutputsPerPayment));

    if (processed.Proof != nullptr) {
        // TODO
        // Are the merkle proofs included? If so, we can accept the tx immediately
        // note: waiting to broadcast the payment and check if it is accepted is not
        // necessarily faster than using the free api with rate limiting. A truly
        // better service would require merkle proofs. Then the tx could be accepted
        // as soon as the proofs were validated, and then broadcast concurrently
        // with the API call.

        // In order to accept merkle proofs, however, we would need a standard format
        // for a bitcoin tx with merkle proofs that can be sent over HTTP. I'm not
        // aware of one.
    }

    // can we broadcast the tx?
    if (!bool (Wallet.net ()->broadcast (processed.Transaction)))
        return make_402 (demanded_price (offer->Price, Parameter.ExpectedFee, Parameter.MaxExpectedOutputsPerPayment));

    // put the outputs in our wallet.
    for (const auto &e : our_new_outputs) Wallet.watch_wallet ()->Account.Account[e.Key] = e.Value;

    // forward the API call.
    return (*offer) ();
}

struct payment_request : Bitcoin::output {
    using Bitcoin::output::output;
    operator std::string () const;
};
