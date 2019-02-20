package httpclient

import (
	"bytes"
	"context"
	"encoding/json"
	"io"
	"io/ioutil"
	"net/http"

	"a.yandex-team.ru/library/go/core/log"
	"a.yandex-team.ru/library/go/core/log/nop"
	"a.yandex-team.ru/yt/go/yson"
	"a.yandex-team.ru/yt/go/yt"
	"a.yandex-team.ru/yt/go/yt/internal"

	"golang.org/x/xerrors"
)

func decodeYTErrorFromHeaders(h http.Header) (ytErr *yt.Error, err error) {
	header := h.Get("X-YT-Error")
	if header == "" {
		return nil, nil
	}

	ytErr = &yt.Error{}
	if decodeErr := json.Unmarshal([]byte(header), ytErr); decodeErr != nil {
		err = xerrors.Errorf("yt: malformed 'X-YT-Error' header: %w", decodeErr)
	}

	return
}

type httpClient struct {
	internal.Encoder

	logger          internal.LoggingInterceptor
	mutationRetrier internal.MutationRetrier
	readRetrier     internal.ReadRetrier

	clusterURL string
	httpClient *http.Client
	log        log.Logger
	config     *yt.Config

	credentials yt.Credentials
}

func (c *httpClient) pickHeavyProxy(ctx context.Context) (string, error) {
	req, err := http.NewRequest("GET", c.clusterURL+"/hosts", nil)
	if err != nil {
		return "", err
	}

	var rsp *http.Response
	rsp, err = c.httpClient.Do(req.WithContext(ctx))
	if err != nil {
		select {
		case <-ctx.Done():
			err = ctx.Err()
		default:
		}
	}
	defer func() { _ = rsp.Body.Close() }()

	if rsp.StatusCode != 200 {
		return "", unexpectedStatusCode(rsp)
	}

	var proxies []string
	if err = json.NewDecoder(rsp.Body).Decode(&proxies); err != nil {
		return "", err
	}

	if len(proxies) == 0 {
		return "", xerrors.New("proxy list is empty")
	}

	best := "http://" + string(proxies[0])
	return best, nil
}

func (c *httpClient) writeParams(h http.Header, call *internal.Call) error {
	var params bytes.Buffer

	w := yson.NewWriter(&params)
	w.BeginMap()
	call.Params.MarshalHTTP(w)
	w.EndMap()
	if err := w.Finish(); err != nil {
		return err
	}

	h.Add("X-YT-Header-Format", "yson")
	h.Add("X-YT-Parameters", params.String())
	h.Add("X-YT-Correlation-ID", call.CallID.String())

	return nil
}

func (c *httpClient) writeHttpRequest(ctx context.Context, call *internal.Call, body io.Reader) (req *http.Request, err error) {
	url := c.clusterURL
	if call.Params.HTTPVerb().IsHeavy() {
		url, err = c.pickHeavyProxy(ctx)
		if err != nil {
			return nil, err
		}
	}

	if body == nil {
		body = bytes.NewBuffer(call.YSONValue)
	}

	verb := call.Params.HTTPVerb()
	req, err = http.NewRequest(verb.HttpMethod(), url+"/api/v4/"+verb.String(), body)
	if err != nil {
		return
	}

	if err = c.writeParams(req.Header, call); err != nil {
		return
	}

	if body != nil {
		req.Header.Add("X-YT-Input-Format", "yson")
	}
	req.Header.Add("X-YT-Output-Format", "yson")

	if c.credentials != nil {
		c.credentials.Set(req)
	}

	return
}

// unexpectedStatusCode is last effort attempt to get useful error message from a failed request.
func unexpectedStatusCode(rsp *http.Response) error {
	if body, err := ioutil.ReadAll(rsp.Body); err == nil {
		var ytErr yt.Error
		if err = json.Unmarshal(body, &ytErr); err == nil {
			return &ytErr
		}
	}

	return xerrors.Errorf("unexpected status code %d", rsp.StatusCode)
}

func (c *httpClient) readResult(rsp *http.Response) (res *internal.CallResult, err error) {
	defer func() { _ = rsp.Body.Close() }()

	res = &internal.CallResult{}

	res.Err, err = decodeYTErrorFromHeaders(rsp.Header)
	if err != nil {
		return
	}
	if res.Err != nil {
		return nil, res.Err
	}

	if rsp.StatusCode/100 != 2 {
		return nil, unexpectedStatusCode(rsp)
	}

	res.YSONValue, err = ioutil.ReadAll(rsp.Body)
	return
}

func (c *httpClient) do(ctx context.Context, call *internal.Call) (res *internal.CallResult, err error) {
	var req *http.Request
	req, err = c.writeHttpRequest(ctx, call, nil)
	if err != nil {
		return nil, err
	}

	var rsp *http.Response
	rsp, err = c.httpClient.Do(req.WithContext(ctx))
	if err != nil {
		select {
		case <-ctx.Done():
			err = ctx.Err()
		default:
		}
	}

	if err == nil {
		res, err = c.readResult(rsp)
	}

	return
}

func (c *httpClient) doWrite(ctx context.Context, call *internal.Call) (w io.WriteCloser, err error) {
	pr, pw := io.Pipe()
	errChan := make(chan error, 1)

	req, err := c.writeHttpRequest(ctx, call, ioutil.NopCloser(pr))
	if err != nil {
		return nil, err
	}

	go func() {
		defer close(errChan)

		rsp, err := c.httpClient.Do(req.WithContext(ctx))
		closeErr := func(err error) {
			errChan <- err
			_ = pr.CloseWithError(err)
		}

		if err != nil {
			closeErr(err)
			return
		}

		defer func() { _ = rsp.Body.Close() }()

		if rsp.StatusCode/100 == 2 {
			return
		}

		callErr, err := decodeYTErrorFromHeaders(rsp.Header)
		if err != nil {
			closeErr(err)
			return
		}

		if callErr != nil {
			closeErr(callErr)
			return
		}

		closeErr(unexpectedStatusCode(rsp))
	}()

	w = &httpWriter{p: pw, errChan: errChan}
	return
}

func (c *httpClient) doWriteRow(ctx context.Context, call *internal.Call) (w yt.TableWriter, err error) {
	var ww io.WriteCloser
	ww, err = c.doWrite(ctx, call)
	if err != nil {
		return
	}

	w = newTableWriter(ww)
	return
}

func (c *httpClient) doReadRow(ctx context.Context, call *internal.Call) (r yt.TableReader, err error) {
	var rr io.ReadCloser
	rr, err = c.doRead(ctx, call)
	if err != nil {
		return
	}

	r = newTableReader(rr)
	return
}

func (c *httpClient) doRead(ctx context.Context, call *internal.Call) (r io.ReadCloser, err error) {
	var req *http.Request
	req, err = c.writeHttpRequest(ctx, call, nil)
	if err != nil {
		return nil, err
	}

	var rsp *http.Response
	rsp, err = c.httpClient.Do(req.WithContext(ctx))
	if err != nil {
		return nil, err
	}

	if rsp.StatusCode != 200 {
		defer func() { _ = rsp.Body.Close() }()

		var callErr *yt.Error
		callErr, err = decodeYTErrorFromHeaders(rsp.Header)
		if err == nil && callErr != nil {
			err = callErr
		} else {
			err = unexpectedStatusCode(rsp)
		}
	}

	if err == nil {
		r = &httpReader{body: rsp.Body, rsp: rsp}
	}

	return
}

func (c *httpClient) Begin(ctx context.Context, options *yt.StartTxOptions) (yt.Tx, error) {
	return internal.NewTx(ctx, c.Encoder, options)
}

func NewHTTPClient(c *yt.Config) (yt.Client, error) {
	var client httpClient

	if c.Logger != nil {
		client.log = c.Logger
	} else {
		client.log = &nop.Logger{}
	}

	client.config = c
	client.clusterURL = yt.NormalizeProxyURL(c.Proxy)
	client.httpClient = http.DefaultClient

	client.Encoder.Invoke = client.do
	client.Encoder.InvokeRead = client.doRead
	client.Encoder.InvokeReadRow = client.doReadRow
	client.Encoder.InvokeWrite = client.doWrite
	client.Encoder.InvokeWriteRow = client.doWriteRow

	if c.Token != "" {
		client.credentials = &yt.TokenCredentials{c.Token}
	}

	return &client, nil
}
