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

#include <data/net/asio/periodic_timer.hpp>

// All the data of the program is collected here.
struct pay_per_call : std::enable_shared_from_this<pay_per_call> {
    parameters Parameter;

    pay_per_call (asio::io_context &ioc, Cosmos::context &, const arg_parser &p);

    // the calls performed by the server in response to HTTP requests.
    net::HTTP::response call (const net::HTTP::request &req);

    // this represents a single call that we support as a paid service.
    struct offer {
        // the price to be paid to make the call.
        Bitcoin::satoshi Price;
        offer (const Bitcoin::satoshi &p): Price {p} {}
        // make the call
        virtual net::HTTP::response operator () () = 0;
        virtual ~offer () {}
    };

private:
    asio::io_context &IOC;

    // Cosmos::context is the wallet and the connection to the Bitcoin network.
    // not necessarily a great interface ATM but it works ok.
    Cosmos::context &Wallet;

    // we will keep track of addresses we have provided to users.
    struct address_data {
        // how to find the key that redeems this address.
        Cosmos::derivation Derivation;
        // when was the address derived.
        time_point DerivationTime;
        // when was the address last seen.
        time_point LastSeen;
    };

    // set of active addresses.
    std::map<digest160, address_data> Addresses;

    // check a url to see if we offer it as a service.
    // if not, nullptr will be returned. Otherwise, we
    // will get a price and a function to call if the
    // payment is valid.
    ptr<offer> offers (const net::URL &);

    // a timer to remove inactive addresses.
    ptr<net::asio::periodic_timer> Timer;

    struct payment_address {
        Bitcoin::address Address;
        Bitcoin::timestamp Expiration;
    };

    payment_address get_new_address ();

    // make a new 402 response to send back to a user for a given price.
    net::HTTP::response make_402 (Bitcoin::satoshi);

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

        // we don't need to spend any coins for this application,
        // so we only load the watch wallet.
        Cosmos::read_watch_wallet_options (Wallet, p);

        // when we run this, all async calls will begin operation.
        boost::asio::io_context ioc;

        auto program = std::make_shared<pay_per_call> (ioc, Wallet, p);
        net::HTTP::server server {ioc, program->Parameter.Endpoint,
            [program] (const net::HTTP::request &req) -> net::HTTP::response {
                return program->call (req);
            }};

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

Bitcoin::satoshi inline round_up (double sats) {
    return Bitcoin::satoshi {int64 (sats + .5)};
}

#include <gigamonkey/script/pattern/pay_to_address.hpp>

using pay_to_address = Gigamonkey::pay_to_address;

// add enough extra in case the user wants to send a payment in up to expected_outputs outputs for some reason.
Bitcoin::satoshi inline demanded_price (Bitcoin::satoshi base_price, satoshi_per_byte expected_fee, uint32 expected_outputs) {
    return base_price + round_up (10 * double (expected_fee) * pay_to_address::redeem_expected_size ());
}

#include <gigamonkey/pay/SVP_envelope.hpp>

// this is what happens every time a user makes a request.
net::HTTP::response pay_per_call::call (const net::HTTP::request &req) {
    // do we recognize this call? If not, send a 404.
    ptr<offer> o = offers (req.URL);
    if (o == nullptr) return net::HTTP::response {net::HTTP::status {404}, {}, ""};

    // the transaction that is being paid to us.
    Bitcoin::transaction tx;

    // Try to read as an SPV_envelope https://tsc.bsvblockchain.org/standards/transaction-ancestors/
    Gigamonkey::nChain::SPV_envelope spv_tx {req.Body};

    bool proofs_included = spv_tx.validate (*Wallet.spvdb ());
    if (proofs_included) tx = Bitcoin::transaction {spv_tx.RawTx};
    // if we can't read an SPV proof, try to read the body as a tx in hex.
    else if (maybe<bytes> raw = encoding::hex::read (req.Body); bool (raw)) tx = Bitcoin::transaction {*raw};
    // otherwise try to read the body as a tx in raw bytes.
    else tx = Bitcoin::transaction {bytes (req.Body)};

    // if we can't read a transaction in the body of the request, send a 402 error.
    if (!tx.valid ()) return make_402 (demanded_price (o->Price, Parameter.ExpectedFee, Parameter.MaxExpectedOutputsPerPayment));

    Bitcoin::txid id = tx.id ();

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
        pay_to_address::redeem_expected_size ()) < o->Price)
        return make_402 (demanded_price (o->Price, Parameter.ExpectedFee, Parameter.MaxExpectedOutputsPerPayment));

    if (proofs_included) {
        // post to the io context a function that broadcasts the tx and put it into our wallet.
        asio::post (IOC, [self = shared_from_this (), tx] () {
            if (!self->Wallet.net ()->broadcast (bytes (tx))) {
                // We've been scammed and that shouldn't be possible.
                // TODO make a log or send myself an email.
            }
        });
    } else {
        // can we broadcast the tx?
        if (!bool (Wallet.net ()->broadcast (bytes (tx))))
            return make_402 (demanded_price (o->Price, Parameter.ExpectedFee, Parameter.MaxExpectedOutputsPerPayment));

        // put the outputs in our wallet.
        for (const auto &e : our_new_outputs) Wallet.watch_wallet ()->Account.Account[e.Key] = e.Value;
    }

    // forward the API call.
    return (*o) ();
}

// make a new 402 response to send back to a user for a given price.
net::HTTP::response pay_per_call::make_402 (Bitcoin::satoshi price) {
    payment_address addr = get_new_address ();

    JSON::object_t payment_request {};
    payment_request ["value"] = Cosmos::write (N (price));
    payment_request ["address"] = std::string (addr.Address);
    payment_request ["expiration"] = uint32 (addr.Expiration);
    payment_request ["memo"] = std::string {} +
        "please include the given payment of the given amount to the given address " +
        "the next time you make this call.";

    return net::HTTP::response {
        net::HTTP::status (402),
        {{net::HTTP::header::content_type, "application/json"}},
        JSON (payment_request).dump ()};
}

pay_per_call::payment_address pay_per_call::get_new_address () {
    auto &pubkeys = Wallet.watch_wallet ()->Pubkeys;
    auto *p = pubkeys.Map.contains (pubkeys.Receive);
    auto next = p->last ().address ();
    *p = p->next ();

    auto now = Bitcoin::timestamp::now ();
    Bitcoin::address new_addr {Bitcoin::address::main, next.Key};
    Addresses[next.Key] = address_data {next.Value.Derivation.first (), time_point (now), time_point (now)};
    return payment_address {new_addr,
        Bitcoin::timestamp {uint32 (now) + static_cast<uint32> (Parameter.InactiveAddressExpiration.count () / 1000)}};
}

// setup the system to remove expired addresses periodically.
pay_per_call::pay_per_call (asio::io_context &ioc, Cosmos::context &wallet, const arg_parser &p):
    IOC {ioc}, Wallet {wallet}, Addresses {},
    Timer {new net::asio::periodic_timer {ioc,
        // run every 10 seconds.
        boost::posix_time::time_duration {0, 0, 10, 0},
        [self = shared_from_this ()] () {
            // go through all the addresses and remove expired.
            time_point now = std::chrono::system_clock::now ();
            for (auto it = self->Addresses.cbegin (); it != self->Addresses.cend ();)
                if (now - it->second.LastSeen < self->Parameter.InactiveAddressExpiration ||
                    now - it->second.DerivationTime < self->Parameter.AbsoluteAddressExpiration
                ) it = self->Addresses.erase (it);
                else ++it;
        }
    }}, Parameter {p} {}

// right now we only forward calls to whatsonchain so this is the only kind of offer we have.
struct whats_on_chain_offer : pay_per_call::offer {
    net::HTTP::REST::request Request;
    net::HTTP::client_blocking &WhatsOnChain;

    whats_on_chain_offer (Bitcoin::satoshi price, const net::HTTP::REST::request &r, Cosmos::network &n) :
        pay_per_call::offer {price}, Request {r}, WhatsOnChain {n.WhatsOnChain} {}

    net::HTTP::response operator () () final override {
        return WhatsOnChain (WhatsOnChain.REST (Request));
    }
};

ptr<pay_per_call::offer> pay_per_call::offers (const net::URL &u) {
    auto path = u.path ().read ();
    if (path.size () == 0 || path.first () != "api.whatsonchain.com") return nullptr;
    return std::static_pointer_cast<pay_per_call::offer> (std::make_shared<whats_on_chain_offer> (
        Bitcoin::satoshi {2000}, net::HTTP::REST::request
            {net::HTTP::method::get, path, u.query_map (), u.fragment (), {}, ""}, *Wallet.net ()));
}

parameters::parameters (const io::arg_parser &p) {
    maybe<string> endpoint;
    p.get ("endpoint", endpoint);
    if (!bool (endpoint)) throw exception {} << "must provide a TCP endpoint to listen from";

    Endpoint = net::IP::TCP::endpoint {*endpoint};

    maybe<uint32> inactive_address_expiration_seconds;
    p.get ("inactive_address_expiration", inactive_address_expiration_seconds);
    if (bool (inactive_address_expiration_seconds))
        InactiveAddressExpiration = duration {1000 * *inactive_address_expiration_seconds};

    maybe<uint32> absolute_address_expiration_seconds;
    p.get ("absolute_address_expiration", absolute_address_expiration_seconds);
    if (bool (absolute_address_expiration_seconds))
        AbsoluteAddressExpiration = duration {1000 * *absolute_address_expiration_seconds};

    maybe<uint32> max_expected_outputs_per_payment;
    p.get ("max_expected_outputs_per_payment", max_expected_outputs_per_payment);
    if (bool (max_expected_outputs_per_payment))
        MaxExpectedOutputsPerPayment = *max_expected_outputs_per_payment;

    maybe<uint32> expected_fee_per_kilobyte;
    p.get ("expected_fee_per_kilobyte", expected_fee_per_kilobyte);
    if (bool (expected_fee_per_kilobyte))
        ExpectedFee = satoshi_per_byte {*expected_fee_per_kilobyte, 1024};

}
