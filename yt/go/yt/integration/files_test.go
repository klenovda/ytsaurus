package integration

import (
	"context"
	"io/ioutil"
	"testing"
	"time"

	"a.yandex-team.ru/yt/go/yttest"

	"a.yandex-team.ru/yt/go/yt"
	"github.com/stretchr/testify/require"
)

func TestFiles(t *testing.T) {
	t.Parallel()

	env, cancel := yttest.NewEnv(t)
	defer cancel()

	ctx, cancel := context.WithTimeout(context.Background(), time.Second*15)
	defer cancel()

	t.Run("P", func(t *testing.T) {
		t.Run("WriteReadFile", func(t *testing.T) {
			t.Parallel()

			name := tmpPath()

			_, err := env.YT.CreateNode(ctx, name, yt.NodeFile, nil)
			require.NoError(t, err)

			w, err := env.YT.WriteFile(ctx, name, nil)
			require.NoError(t, err)

			_, err = w.Write([]byte("test"))
			require.NoError(t, err)
			require.NoError(t, w.Close())

			r, err := env.YT.ReadFile(ctx, name, nil)
			require.NoError(t, err)
			defer r.Close()

			file, err := ioutil.ReadAll(r)
			require.Equal(t, file, []byte("test"))
		})

		t.Run("ReadFileError", func(t *testing.T) {
			t.Parallel()

			name := tmpPath()

			_, err := env.YT.ReadFile(ctx, name, nil)
			require.Error(t, err)
			require.True(t, yt.ContainsErrorCode(err, 500))
		})

		t.Run("WriteFileError", func(t *testing.T) {
			t.Parallel()

			name := tmpPath()

			w, err := env.YT.WriteFile(ctx, name, nil)
			require.NoError(t, err)

			err = w.Close()
			require.Error(t, err)
			require.True(t, yt.ContainsErrorCode(err, 500))
		})
	})
}
